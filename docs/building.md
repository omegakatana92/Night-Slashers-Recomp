# Building and packaging

## Common steps

```sh
git submodule update --init
python3 tools/verify_rom.py rom/nslasher.zip
python3 tools/recompile.py rom/nslasher.zip    # seeds from tools/traced_targets.txt
```

`recompile.py` writes `build/generated/` (translated program, `program_id.c` with the program
CRC used by save states and ROM checks). It must be re-run after changing `tools/arm2/` or
`tools/traced_targets.txt`.

## Linux (desktop)

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j
```

## Windows x64

MSVC with the SDL2 VC development package:

```powershell
cmake -S . -B build\vs -G "Visual Studio 18 2026" -A x64 -DSDL2_DIR=C:\SDL2-2.30.9\cmake
cmake --build build\vs --config Release
copy C:\SDL2-2.30.9\lib\x64\SDL2.dll build\vs\bin\
```

Cross-compiling from Linux with MinGW-w64 (used for the 0.2.0 package; the executables link
libgcc/libstdc++/winpthread statically and need only `SDL2.dll`):

```sh
cmake -S . -B build/win64 -DCMAKE_TOOLCHAIN_FILE=cmake-mingw64.cmake -DCMAKE_BUILD_TYPE=Release \
      -DSDL2_DIR=<SDL2-2.30.9 mingw>/x86_64-w64-mingw32/lib/cmake/SDL2 \
      -DCMAKE_FIND_ROOT_PATH=<SDL2-2.30.9 mingw>/x86_64-w64-mingw32
cmake --build build/win64 -j
```

Package: `nightslashers.exe`, `nsl_headless.exe`, `SDL2.dll`, `LICENSE`, `THIRD_PARTY.md`,
`LICENSES/`, `USER_GUIDE.md`. No ROM.

## Steam Deck / Linux AppImage

```sh
scripts/package_appimage.sh <SDL2 prefix> <appimagetool>
```

The SDL2 prefix should be SDL 2.30.x built with its video/audio back ends loaded at run time
(the default `*_SHARED` options), so the bundled `libSDL2-2.0.so.0` depends only on libc/libm
and uses the host's X11, Wayland, PipeWire-Pulse and ALSA libraries. To keep the glibc floor
low when building on a new distribution, SDL 0.2.0 was configured with:

```sh
printf '#ifndef _GNU_SOURCE\n#define _GNU_SOURCE 1\n#endif\n#include <features.h>\n#undef __GLIBC_USE_C2X_STRTOL\n#define __GLIBC_USE_C2X_STRTOL 0\n' > glibc_floor.h
cmake -S SDL2-2.30.9 -B sdlbuild -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=OFF -DSDL_TEST=OFF \
      -DCMAKE_C_FLAGS="-include $PWD/glibc_floor.h" \
      -DLIBC_HAS_STRLCPY=0 -DLIBC_HAS_STRLCAT=0 -DLIBC_HAS_WCSLCPY=0 -DLIBC_HAS_WCSLCAT=0 \
      -DLIBC_HAS_FMOD=0 -DLIBC_HAS_FMODF=0 -DCMAKE_INSTALL_PREFIX=$PWD/sdl2-linux
```

Dependency audit of the 0.2.0 AppImage (`objdump -T`, `readelf -d`):

| File | Needs |
| --- | --- |
| `usr/bin/nightslashers` | `libSDL2-2.0.so.0` (bundled), `libstdc++.so.6` (GLIBCXX_3.4.21), `libgcc_s.so.1`, `libm`, `libc` — glibc ≤ 2.34 |
| `usr/lib/libSDL2-2.0.so.0` | `libm`, `libc` — glibc ≤ 2.34; X11/Wayland/audio libraries opened at run time |

libstdc++ is deliberately left to the host: a static copy from a new toolchain pulls in
glibc 2.35–2.38 symbols. SteamOS 3.5 and later meet the glibc 2.34 floor.

Settings and saves go to `~/.local/share/NightSlashersRecomp`; `AppRun` selects Wayland
only when no X11 display exists (Gamescope provides XWayland, so X11 is normally used).
