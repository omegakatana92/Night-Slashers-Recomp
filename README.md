# Night Slashers Native Recompilation

A native port of **Night Slashers** (Data East, 1994) for Windows 10/11 x64 and Linux
x86-64 / Steam Deck. The game's ARM (DE156) program is decrypted from **your own ROM** and
translated ahead of time into C, then compiled with a native implementation of the DECO32
board. No ROM data is stored in this repository or in release packages.

**Status: playable (0.2.0).** Boots, plays and sounds like MAME's reference output in every
check run so far. See [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md) and the
[user guide](docs/USER_GUIDE.md).

| Area | State |
| --- | --- |
| ROM loading (zip or folder), CRC checks, clear errors | Implemented, tested |
| DECO156 program / DECO56+74 tile decryption (C runtime) | Implemented, tested (equal to the Python reference, which matches MAME) |
| ARM2 → C translation + fallback for unseen code | Implemented, tested (translator and fallback give bit-identical runs) |
| Board: bus, DECO104, 93C46 EEPROM, IRQ/VBLANK timing | Implemented, tested |
| Video: DECO16IC ×2, DECO 52 sprites ×2, ACE palette/fades/alpha mixer | Implemented, tested against MAME frames |
| Audio: Z80, YM2151 (ymfm), OKI M6295 ×2 | Implemented, measured against MAME output (level/spectrum) |
| Save states (8 slots + autosave, previews, validation) | Implemented, tested (bit-exact round trips, damaged/incompatible rejection) |
| SDL2 front end: menus, quick menu, ROM picker, settings | Implemented, tested on a virtual display (Linux and Windows/Wine) |
| Controllers, hotplug, remapping | Implemented, **not tested with physical controllers** |
| Windows x64 package / Steam Deck AppImage | Built; **not tested on real Windows 11 or a Steam Deck** |

## Building

Requirements: Python 3.10+, CMake 3.20+, a C11/C++14 compiler, SDL2 development files.

```sh
python3 tools/verify_rom.py rom/nslasher.zip
python3 tools/recompile.py rom/nslasher.zip          # writes build/generated (never commit)
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j
build/linux/bin/nightslashers --rom rom/nslasher.zip
```

Windows: MSVC (`cmake -S . -B build\vs -G "Visual Studio 18 2026" -A x64 -DSDL2_DIR=<SDL2 VC
dev package>\cmake`) or cross-compile with MinGW-w64 (`-DCMAKE_TOOLCHAIN_FILE=cmake-mingw64.cmake`).
Release packaging, the Steam Deck AppImage and the glibc baseline are described in
[`docs/building.md`](docs/building.md).

The translated C is produced locally from the player's ROM and is never committed.

## Tests

```sh
build/linux/bin/nsl_headless --frames 3600 --hashes out.txt          # deterministic attract mode
build/linux/bin/nsl_headless --frames 3000 --coin-at 200 --start-at 300 --autoplay 7   # gameplay
tests/ui_smoke.sh build/linux/bin/nightslashers rom/nslasher.zip /tmp/ui              # front end
```

`--save-state-at N FILE`, `--load-state FILE`, `--png-every N --png-dir DIR` and `--wav FILE`
support the round-trip and reference comparisons recorded in `docs/milestones/`.

## Layout

| Path | Contents |
| --- | --- |
| `tools/` | ROM verification, DECO156 decryption, ARM2 decoder/analysis/translator (`tools/arm2`), seeds |
| `runtime/` | core API, CPU helpers, fallback, board, video, sound, ROM loader, save states |
| `ports/headless` | deterministic test runner |
| `ports/sdl2` | front end: menus, input, display, audio, states, rewind, recording, gadgets |
| `scripts/`, `packaging/` | AppImage packaging, desktop entry and original icon |
| `docs/` | user guide, hardware notes, known issues, milestones, licensing |

## Licence

Original code: GPL-2.0-only (`LICENSE`). Third-party components: [`THIRD_PARTY.md`](THIRD_PARTY.md).
Night Slashers is © Data East Corporation; this project is not affiliated with the rights holders.
