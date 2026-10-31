# Night Slashers Native Recompilation

A native port of **Night Slashers** (Data East, 1994) for Windows 11 x64 and SteamOS/Linux
x86-64, built from the player's own ROM with the ahead-of-time translation approach of
[`cheviman/arcade-decomp`](https://github.com/cheviman/arcade-decomp).

This repository is a **work in progress**, not a playable release. The ROM verifier, DECO156
program decryption, ARM2 frontend, static analysis, C translation and board runtime are
implemented and tested as far as documented below. The game currently stops during its
boot-time EEPROM/settings check before the first playable frame; see
[`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md). No `v1.0.0` playable release exists.

| Area | State |
| --- | --- |
| ROM verification, parent and Japan | **Working** — 17/17 required files, CRC32 + SHA-1 |
| DECO156 program decryption | **Working** — byte-for-byte equal to MAME 0.289's decrypted image |
| ARM2 (DE156) decoder / disassembler | **Working** — ARM2 26-bit subset, coprocessor 0, no Thumb |
| Static analysis / basic blocks | **Working** — 8,046 instructions, 1,506 blocks from vectors and branches |
| AOT translation to C | **Working** — no instruction decoding at run time |
| CPU/bus/DECO104/EEPROM runtime | **Partial** — boot reaches late graphics/EEPROM initialisation |
| Video (DECO16IC, sprites, ACE) | **Not implemented** (black frame) |
| Audio (Z80, YM2151, 2×OKI6295) | **Not implemented** |
| Input, SDL2 front end, AppImage | **Not implemented** |
| Playable build / v1.0.0 | **Not achieved** |

## Quick start (development)

Requirements: Python 3.11+, CMake 3.20+, a C11 compiler (MSVC 2022+ or GCC/Clang), Ninja or
Visual Studio. The arcade-decomp core is a pinned submodule in `tools/core`.

```powershell
# 1. Verify your own ROM archive (never commit it)
python tools\verify_rom.py rom\nslasher.zip
python tools\verify_rom.py rom\nslasherj.zip

# 2. Translate the program (writes build/generated and build/assets/program.bin)
python tools\recompile.py rom\nslasher.zip

# 3. Configure and build the headless harness
cmake -S . -B build\vs -G "Visual Studio 18 2026" -A x64
cmake --build build\vs --config Release

# 4. Run it
build\vs\bin\nsl_headless.exe --frames 60
```

A MinGW/Ninja build is also supported:

```powershell
cmake -S . -B build\mingw -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe
cmake --build build\mingw
build\mingw\bin\nsl_headless.exe --frames 60
```

The generated C is the game's translated program and is **not** committed. Only the player's
ROM can produce it, locally.

## ROM sets

| MAME set | Archive | Region | Program ROMs |
| --- | --- | --- | --- |
| `nslasher` (parent) | `nslasher.zip` | Korea Ver. 1.3, DE-0397-0 | `mainprg.1f`, `mainprg.2f` |
| `nslasherj` (clone) | `nslasherj.zip` | Japan Ver. 1.2, DE-0397-0 | `lx-00.1f`, `lx-01.2f` |

Both are supported by the verifier and the loader. The two sets differ only in the main
program ROM pair; graphics, sound, PLD and PROM files are identical. `vm-00.3d` and
`vm-01.4d` are `NO_DUMP` in MAME and are not required.

## What is implemented

- **ROM tooling** (`tools/nsl_rom.py`, `tools/verify_rom.py`): archive inventory, CRC32,
  SHA-1, exact MAME set identification, clone/parent relationship, required-file checks,
  program interleaving, DECO156 decryption, DECO56/DECO74 graphics decryption.
- **ARM2 frontend** (`tools/arm2/`): complete ARM2 data-processing, multiply, single and
  block transfer, branch, coprocessor and SWI decoding; disassembler; recursive static
  analysis with literal-pool tracking; C emitter with 26-bit PC/PSR semantics, banked
  registers, exception entry/return, shifter carry, and the DE156 BCD/divide coprocessor.
- **Runtime** (`runtime/`): DE156 CPU state and helpers, 26-bit bus, 128 KiB work RAM,
  DECO104 protection/input table, DECO ACE palette buffer and DMA, sprite buffers and DMA,
  DECO16IC VRAM/row-scroll/control backing stores, 93C46 EEPROM model, frame scheduler and
  headless hash harness.
- **Shared toolchain**: `tools/core` is the arcade-decomp core pinned at
  `ed4e157b4bb3d34b6555766262c70d6f010ba619` (branch `tmnt`) for its Z80 frontend and
  translation conventions.

## Verification that was actually run

```
python tools/verify_rom.py nslasher.zip   -> OK: all 17 required files match MAME nslasher
python tools/verify_rom.py nslasherj.zip  -> OK: all 17 required files match MAME nslasherj
decrypted program (parent)   SHA-1 a366c99365194ccdfcb9c573bb0b739d985b8f68
decrypted program (Japan)    SHA-1 7faaf85ca833331f9af1993e9846bf88992e9ae8
decryption vs MAME 0.289 dump: 65,536 / 65,536 words equal
headless 60 frames: no CPU fault, reaches PC 0x3E978 / 0x3EB74, palette and sprite DMA seen
```

The exact boot blocker and the evidence collected so far are in
[`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md).

## Layout

```
CMakeLists.txt              shared Windows/Linux build
tools/
  core/                     arcade-decomp, pinned submodule (BSD-3-Clause)
  arm2/                     ARM2 decoder, analysis, translator, disassembler
  nsl_rom.py                ROM sets, verification, DECO156/DECO graphics decryption
  verify_rom.py             player-facing ROM checker
  recompile.py              ROM -> build/generated/program_*.c
runtime/
  include/nsl/              CPU, machine and run interfaces
  src/                      CPU, bus/board devices, video, scheduler
ports/headless/             deterministic frame runner
docs/                       hardware, feasibility, status and milestone notes
tests/                      tool self-checks
```

## Licence and ROM policy

The repository's existing `LICENSE` (GNU GPL v2.0) is preserved pending the licensing review
requested for this project. The port's own source files carry `SPDX-License-Identifier:
GPL-2.0-only`. The pinned arcade-decomp core is BSD-3-Clause and remains under its own licence;
MAME-derived tables and behaviour notes are BSD-3-Clause. See `LICENSES/` and
[`docs/licensing.md`](docs/licensing.md).

No ROM, extracted ROM data, generated translated C or copyrighted art/audio is committed.
`rom/`, `build/` and local EEPROM/NVRAM files are ignored by Git.
