# AGENTS.md — Night Slashers Native Recompilation

## Mission

Build genuine native ports of **Night Slashers** (Data East, 1994, ROM set `nslasher` and its
Japan clone `nslasherj`) for Windows 11 x64 and SteamOS/Linux x86-64, with the ahead-of-time
translation architecture of `cheviman/arcade-decomp`.

This is **not** an emulator and must not ship an emulator core. The ARM2 program is decrypted
from the player's ROM, translated ahead of time into C and compiled to native code. The
runtime implements the DECO32 board directly.

## Non-negotiables

1. **Never commit ROMs** (`rom/`, `*.zip`), extracted art/audio, generated translated C
   (`build/generated/`), EEPROM/NVRAM dumps or credentials.
2. **Never claim success without running the check.** Record the command and output under
   `docs/milestones/`.
3. Keep `tools/core` a pristine submodule at the pinned commit. Put game-specific and new
   CPU frontend code under `tools/` in this repository.
4. Preserve the existing `LICENSE` (GPL-2.0) until the licensing review is complete. Keep
   BSD-3-Clause notices for arcade-decomp and MAME-derived material.
5. If a directory is not writable, stop and report it.

## Hardware target

- DE156: ARM2 in 26-bit mode, 7 MHz, encrypted 1 MiB program.
- 128 KiB work RAM, 320×240 at 57.828 Hz, DECO16IC tile generators, two DECO sprite
  generators, DECO ACE palette, DECO104 protection/I/O, 93C46 EEPROM.
- Z80 sound CPU at 3.58 MHz, YM2151, two OKI M6295.
- See `docs/hardware.md`.

## Build and test

```powershell
python tools\verify_rom.py rom\nslasher.zip
python tools\recompile.py rom\nslasher.zip
cmake -S . -B build\vs -G "Visual Studio 18 2026" -A x64
cmake --build build\vs --config Release
build\vs\bin\nsl_headless.exe --frames 60 --hashes build\boot.hashes
```

A change is not complete unless ROM verification, translation without `Unsupported`, and a
headless run are recorded. Do not publish a release while `docs/KNOWN_ISSUES.md` reports the
boot blocker.

## Style

- C11; explicit little-endian handling for ARM words and big-endian 16-bit words for DECO
  graphics.
- `lower_snake_case` for C; board constants `NSL_`; 68000/ARM constants as appropriate.
- Comment the MAME file/function each hardware behaviour follows.
