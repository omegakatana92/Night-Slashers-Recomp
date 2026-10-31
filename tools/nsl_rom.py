"""
Name:        nsl_rom.py
Description: Night Slashers ROM sets, verification, DECO156 program decryption and ROM image building
Version:     0.1.0
Created:     31-10-2026

The player supplies nslasher.zip (parent, MAME "Night Slashers (Korea Ver. 1.3)") or
nslasherj.zip (Japan Ver. 1.2). Nothing here contains ROM data: it reads the player's archive
and builds the images the board runtime needs.

The file list, CRCs and SHA-1s are MAME 0.289's src/mame/dataeast/fghthist.cpp (ROM_START(nslasher)
and ROM_START(nslasherj)), BSD-3-Clause. The DECO156 decryption is MAME's
src/mame/dataeast/deco156.cpp (BSD-3-Clause) ported to Python and checked word for word against
MAME's decrypted image (65,536/65,536 words on the parent set).
"""
from __future__ import annotations

import hashlib
import io
import struct
import zipfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path

PROGRAM_SIZE = 0x100000
WORK_RAM = (0x100000, 0x120000)
SOUND_RAM = (0x0000, 0x2000)          # Z80 internal RAM, MAME's z80_sound_map


@dataclass(frozen=True)
class RomFile:
    name: str
    size: int
    crc: int
    sha1: str


PARENT_PROGRAM = (
    RomFile("mainprg.1f", 0x80000, 0x507ACBAE, "329a2bb244f2f3adb8d75cab5aa2dcb129d70712"),
    RomFile("mainprg.2f", 0x80000, 0x931FC7EE, "54eb12abfa3f332ce9b43a45ec424aaee88641a6"),
)
JAPAN_PROGRAM = (
    RomFile("lx-00.1f", 0x80000, 0x6ED5FB88, "84350da7939a479968a523c84e254e3ee54b8da2"),
    RomFile("lx-01.2f", 0x80000, 0xA6DF2152, "6fe7e0b2e71c5f807951dcc81a6a3cff55247961"),
)

COMMON_FILES = (
    RomFile("sndprg.17l", 0x10000, 0x18939E92, "50b37a78d9d2259d4b140dd17393c4e5ca92bca5"),
    RomFile("mbh-00.8c", 0x200000, 0xA877F8A3, "79253525f360a73161894f31e211e4d6b38d307a"),
    RomFile("mbh-01.9c", 0x200000, 0x1853DAFC, "b1183c0db301cbed9f079c782202dbfc553b198e"),
    RomFile("mbh-02.14c", 0x200000, 0xB2F158A1, "4f8c0b324813db198fe1dad7fff4185b828b94de"),
    RomFile("mbh-04.16c", 0x200000, 0xEECFE06D, "2df817fe5e2ea31207b217bb03dc58979c05d0d2"),
    RomFile("mbh-06.18c", 0x100000, 0x038C2127, "5bdb215305f1a419fde27a83b623a38b9328e560"),
    RomFile("mbh-03.15c", 0x080000, 0x787787E3, "531444e3f28aa9a7539a5a76ca94a9d6b97274c5"),
    RomFile("mbh-05.17c", 0x080000, 0x1D2B7C17, "ae0b8e0448a1a8180fb424fb0bc8a4302f8ff602"),
    RomFile("mbh-07.19c", 0x040000, 0xBBD22323, "6ab665b2e6d04cdadc48c52e15098e978b61fe10"),
    RomFile("mbh-08.16e", 0x080000, 0xCDD7F8CB, "910bbe8783c0ba722e9d6399b332d658fa059fdb"),
    RomFile("mbh-09.18e", 0x080000, 0x33FA2121, "eb0e99d29b1ad9995df28e5b7cfc89d53efb53c3"),
    RomFile("mbh-10.14l", 0x080000, 0xC4D6B116, "c5685bce6a6c6a74ca600ebf766ba1007f0dc666"),
    RomFile("mbh-11.16l", 0x080000, 0x0EC40B6B, "9fef44149608ae2a00f6a75a6f77f2efcab6e78e"),
    RomFile("ln-00.j7", 0x200, 0x5E83EAF3, "95f5eb8e56dff6c2dce7c39a6dd458bfc38fe1cf"),
    RomFile("vm-02.8j", 0x117, 0x53692426, "b8f8cf6b1f6b637fcd1fcd62474e637f5d4a6901"),
)

ROM_SETS = {
    "nslasher": {
        "title": "Night Slashers (Korea Ver. 1.3, DE-0397-0 PCB)",
        "mame": "nslasher",
        "parent": None,
        "program": PARENT_PROGRAM,
        "files": PARENT_PROGRAM + COMMON_FILES,
    },
    "nslasherj": {
        "title": "Night Slashers (Japan Ver. 1.2, DE-0397-0 PCB)",
        "mame": "nslasherj",
        "parent": "nslasher",
        "program": JAPAN_PROGRAM,
        "files": JAPAN_PROGRAM + COMMON_FILES,
    },
}

# MAME marks vm-00.3d and vm-01.4d as NO_DUMP PLDs; they are not required.
OPTIONAL_NO_DUMP = ("vm-00.3d", "vm-01.4d")


class RomError(Exception):
    pass


@dataclass
class FileCheck:
    expected: RomFile
    present: bool = False
    size: int = 0
    crc: int = 0
    sha1: str = ""
    data: bytes = b""

    @property
    def crc_ok(self) -> bool:
        return self.present and self.crc == self.expected.crc

    @property
    def sha1_ok(self) -> bool:
        return self.present and self.sha1 == self.expected.sha1

    @property
    def size_ok(self) -> bool:
        return self.present and self.size == self.expected.size

    @property
    def ok(self) -> bool:
        return self.size_ok and self.crc_ok and self.sha1_ok


@dataclass
class RomSet:
    name: str
    title: str
    mame: str
    parent: str | None
    checks: dict[str, FileCheck]
    extra: list[str] = field(default_factory=list)

    @property
    def complete(self) -> bool:
        return all(check.ok for check in self.checks.values())

    @property
    def files(self) -> dict[str, bytes]:
        return {name: check.data for name, check in self.checks.items() if check.present}

    def missing(self) -> list[str]:
        return [name for name, check in self.checks.items() if not check.present]

    def incorrect(self) -> list[str]:
        return [name for name, check in self.checks.items() if check.present and not check.ok]

    def program_words(self) -> list[int]:
        """The 1 MiB program region as little-endian 32-bit words, still encrypted."""
        if not self.complete:
            raise RomError(f"{self.name} is incomplete: missing {self.missing()}, bad {self.incorrect()}")
        one, two = self.program_files()
        if len(one) != 0x80000 or len(two) != 0x80000:
            raise RomError("program ROM pair has the wrong size")
        # MAME ROM_LOAD32_WORD: 16-bit group from the first file at bytes 0-1 of every
        # 32-bit word, the second at bytes 2-3. The ARM is little-endian.
        words = [0] * (PROGRAM_SIZE // 4)
        for index in range(len(words)):
            offset = index * 2
            words[index] = (one[offset] | (one[offset + 1] << 8)
                            | (two[offset] << 16) | (two[offset + 1] << 24))
        return words

    def program_files(self) -> tuple[bytes, bytes]:
        if self.name == "nslasherj":
            return self.files["lx-00.1f"], self.files["lx-01.2f"]
        return self.files["mainprg.1f"], self.files["mainprg.2f"]


def _archive_entries(path: Path) -> dict[str, bytes]:
    data: dict[str, bytes] = {}
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            name = info.filename.replace("\\", "/").rsplit("/", 1)[-1]
            data[name] = archive.read(info)
    return data


def _directory_entries(path: Path) -> dict[str, bytes]:
    return {item.name: item.read_bytes() for item in path.iterdir() if item.is_file()}


def identify(path: str | Path) -> str:
    """Which of the supported sets a zip holds, by its program pair only."""
    path = Path(path)
    if not path.exists():
        raise RomError(f"not found: {path}")
    entries = _archive_entries(path) if path.suffix.lower() == ".zip" else _directory_entries(path)
    for name in ("nslasher", "nslasherj"):
        program = ROM_SETS[name]["program"]
        if all(f.name in entries for f in program):
            return name
    raise RomError("this archive is neither nslasher nor nslasherj (program ROM pair not found)")


def inspect(path: str | Path, set_name: str | None = None) -> RomSet:
    path = Path(path)
    if not path.exists():
        raise RomError(f"not found: {path}")
    entries = _archive_entries(path) if path.suffix.lower() == ".zip" else _directory_entries(path)
    if set_name is None:
        set_name = identify(path)
    if set_name not in ROM_SETS:
        raise RomError(f"unknown set {set_name!r}; expected nslasher or nslasherj")
    spec = ROM_SETS[set_name]
    checks: dict[str, FileCheck] = {}
    for expected in spec["files"]:
        check = FileCheck(expected)
        if expected.name in entries:
            data = entries[expected.name]
            check.present = True
            check.size = len(data)
            check.crc = zlib.crc32(data) & 0xFFFFFFFF
            check.sha1 = hashlib.sha1(data).hexdigest()
            check.data = data
        checks[expected.name] = check
    known = {f.name for f in spec["files"]} | set(OPTIONAL_NO_DUMP)
    extra = sorted(name for name in entries if name not in known)
    return RomSet(set_name, spec["title"], spec["mame"], spec["parent"], checks, extra)


# ---- DECO156 -------------------------------------------------------------

_BS = (
    (1, 4, 7, 28, 22, 18, 20, 9, 16, 10, 30, 2, 31, 24, 19, 29,
     6, 21, 23, 11, 12, 13, 5, 0, 8, 26, 27, 15, 14, 17, 25, 3),
    (14, 23, 28, 29, 6, 24, 10, 1, 5, 16, 7, 2, 30, 8, 18, 3,
     31, 22, 25, 20, 17, 0, 19, 27, 9, 12, 21, 15, 26, 13, 4, 11),
    (19, 30, 21, 4, 2, 18, 15, 1, 12, 25, 8, 0, 24, 20, 17, 23,
     22, 26, 28, 16, 9, 27, 6, 11, 31, 10, 3, 13, 14, 7, 29, 5),
    (30, 6, 15, 0, 31, 18, 26, 22, 14, 23, 19, 17, 10, 8, 11, 20,
     1, 28, 2, 4, 9, 24, 25, 27, 7, 21, 13, 29, 5, 3, 16, 12),
)
_XOR = (0xEC63197A, 0x58A5A55F, 0xE3A65F16, 0x28D93783)
_ADDR_XOR = (
    (0x0001, 0xCE4A), (0x0002, 0x4DB2), (0x0004, 0xEF60), (0x0008, 0x5737),
    (0x0010, 0x13DC), (0x0020, 0x4BD9), (0x0040, 0xA209), (0x0080, 0xD996),
    (0x0100, 0xA700), (0x0200, 0xECA0), (0x0400, 0x7529), (0x0800, 0x3100),
    (0x1000, 0x33B4), (0x2000, 0x6161), (0x4000, 0x1EEF), (0x8000, 0xF5A5),
)
_DATA_XOR = (
    (0x00004, 0x04400000), (0x00008, 0x40000004), (0x00010, 0x00048000),
    (0x00020, 0x00000280), (0x00040, 0x00200040), (0x00080, 0x09000000),
    (0x00100, 0x00001100), (0x00200, 0x20002000), (0x00400, 0x00000022),
    (0x00800, 0x000A0000), (0x01000, 0x10004000), (0x02000, 0x00010400),
    (0x04000, 0x80000010), (0x08000, 0x00000009), (0x10000, 0x02100000),
    (0x20000, 0x00800800),
)


def _bitswap(value: int, positions: tuple[int, ...]) -> int:
    """MAME's bitswap: the first listed source bit becomes the most significant result bit."""
    result = 0
    for position in positions:
        result = (result << 1) | ((value >> position) & 1)
    return result


def deco156_decrypt(words: list[int]) -> list[int]:
    """MAME's deco156_decrypt, word for word. 'words' is the encrypted 1 MiB region."""
    source = list(words)
    result = [0] * len(source)
    for index in range(len(source)):
        address = (index & 0xFF0000) | 0x92C6
        for bit, xor in _ADDR_XOR:
            if index & bit:
                address ^= xor
        value = source[address]
        for bit, xor in _DATA_XOR:
            if index & bit:
                value ^= xor
        result[index] = _bitswap(value ^ _XOR[index & 3], _BS[index & 3])
    return result


def build_program(rom: RomSet) -> bytes:
    """The decrypted program as little-endian bytes, ready for analysis and for the runtime."""
    return b"".join(struct.pack("<I", word) for word in deco156_decrypt(rom.program_words()))


# ---- ROM images the board runtime loads ----------------------------------

def build_tiles(rom: RomSet) -> tuple[bytes, bytes]:
    """tiles1 and tiles2 after MAME's reorder and deco56/deco74 decryption (see decocrpt.cpp)."""
    tiles1 = bytearray(rom.files["mbh-00.8c"])
    tiles2 = bytearray(rom.files["mbh-01.9c"])
    for ram in (tiles1, tiles2):
        # nslasher_state::reorder_tilegfx: swap the 0x080000-0x100000 and 0x100000-0x180000 halves
        temp = bytes(ram[0x080000:0x100000])
        ram[0x080000:0x100000] = ram[0x100000:0x180000]
        ram[0x100000:0x180000] = temp
    deco56_decrypt_gfx(tiles1)
    deco74_decrypt_gfx(tiles2)
    return bytes(tiles1), bytes(tiles2)


def _deco_decrypt_gfx(ram: bytearray, xor_table, address_table, swap_table) -> None:
    """MAME decocrpt.cpp deco_decrypt (BSD-3-Clause), on big-endian 16-bit words."""
    from deco_gfx_tables import xor_masks, swap_patterns
    words = [struct.unpack_from(">H", ram, i)[0] for i in range(0, len(ram), 2)]
    source = list(words)
    for index in range(len(words)):
        address = (index & ~0x7FF) | address_table[index & 0x7FF]
        pattern = swap_patterns[swap_table[index & 0x7FF]]
        value = source[address] ^ xor_masks[xor_table[address & 0x7FF]]
        words[index] = _bitswap(value, pattern)
    for i, word in enumerate(words):
        struct.pack_into(">H", ram, i * 2, word)


def deco56_decrypt_gfx(ram: bytearray) -> None:
    from deco_gfx_tables import deco56_xor_table, deco56_address_table, deco56_swap_table
    _deco_decrypt_gfx(ram, deco56_xor_table, deco56_address_table, deco56_swap_table)


def deco74_decrypt_gfx(ram: bytearray) -> None:
    from deco_gfx_tables import deco74_xor_table, deco74_address_table, deco74_swap_table
    _deco_decrypt_gfx(ram, deco74_xor_table, deco74_address_table, deco74_swap_table)


class ProgramMemory:
    """Big-endian-free little-endian view of the decrypted program, for the analysis tools."""

    def __init__(self, image: bytes):
        self.image = image

    def r8(self, address: int) -> int:
        return self.image[address & 0xFFFFF]

    def r16(self, address: int) -> int:
        address &= 0xFFFFF
        return self.image[address] | (self.image[address + 1] << 8)

    def r32(self, address: int) -> int:
        address &= 0xFFFFF
        return struct.unpack_from("<I", self.image, address)[0]


def load(path: str | Path, set_name: str | None = None) -> RomSet:
    rom = inspect(path, set_name)
    if not rom.complete:
        raise RomError(f"{rom.name} is incomplete: missing {rom.missing()}, bad {rom.incorrect()}")
    return rom
