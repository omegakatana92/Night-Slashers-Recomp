# Third-party components

| Component | Where | Licence | Notes |
| --- | --- | --- | --- |
| arcade-decomp | `tools/core` (submodule, pinned) | BSD-3-Clause | `LICENSES/BSD-3-Clause-arcade-decomp.txt` |
| MAME (reference only) | algorithms/tables in `tools/`, `runtime/src/deco*_table.h`, behaviour notes | BSD-3-Clause | No MAME code is linked; `LICENSES/BSD-3-Clause-MAME.txt` |
| ymfm 81aec25 (Aaron Giles) | `runtime/third_party/ymfm` | BSD-3-Clause | YM2151; `LICENSES/BSD-3-Clause-ymfm.txt` |
| superzazu/z80 | `runtime/third_party/z80` | MIT | Z80 sound CPU; patched to pass 16-bit port addresses; `LICENSES/MIT-superzazu-z80.txt` |
| miniz 3.1.2 | `runtime/third_party/miniz` | MIT | ZIP reading, state compression, PNG; `LICENSES/MIT-miniz.txt` |
| font8x8 (Daniel Hepper) | `ports/sdl2/font8x8_basic.h` | Public domain | Menu font, EPX-upscaled at start-up |
| SDL 2.30.9 | linked / shipped as `SDL2.dll` or `libSDL2-2.0.so.0` | zlib | `LICENSES/Zlib-SDL2.txt` |

Night Slashers is © 1994 Data East Corporation. This project is not affiliated with the rights
holders. No ROM data, graphics, sound or translated program is stored in this repository.
