#!/bin/bash
# Builds build/NightSlashers-x86_64.AppImage (Steam Deck / desktop Linux).
#   scripts/package_appimage.sh <SDL2 prefix built with dynamic backends> <appimagetool>
# The ROM is never included. Translated C must already exist (tools/recompile.py).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDL_PREFIX=${1:?SDL2 install prefix}
TOOL=${2:?path to appimagetool}
BUILD=$ROOT/build/appimage
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DNSL_PORTABLE_LINUX=ON -DSDL2_DIR="$SDL_PREFIX/lib/cmake/SDL2"
cmake --build "$BUILD" -j"$(nproc)"
APPDIR=$ROOT/build/AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/doc/nightslashers"
cp "$BUILD/bin/nightslashers" "$BUILD/bin/nsl_headless" "$APPDIR/usr/bin/"
cp -L "$SDL_PREFIX/lib/libSDL2-2.0.so.0" "$APPDIR/usr/lib/"
strip "$APPDIR/usr/bin/"* "$APPDIR/usr/lib/"*
cp "$ROOT/packaging/nightslashers.desktop" "$APPDIR/"
cp "$ROOT/packaging/nightslashers.png" "$APPDIR/nightslashers.png"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY.md" "$ROOT/docs/USER_GUIDE.md" "$APPDIR/usr/share/doc/nightslashers/"
cp -r "$ROOT/LICENSES" "$APPDIR/usr/share/doc/nightslashers/"
cat > "$APPDIR/AppRun" <<'RUN'
#!/bin/sh
# Settings, saves and EEPROM live in ~/.local/share/NightSlashersRecomp (never inside the image).
HERE=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Gamescope / Gaming Mode: prefer Wayland when available, fall back to X11
if [ -z "${SDL_VIDEODRIVER:-}" ] && [ -n "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then export SDL_VIDEODRIVER=wayland; fi
if [ "$(basename "${ARGV0:-$0}")" = "nsl_headless" ]; then exec "$HERE/usr/bin/nsl_headless" "$@"; fi
exec "$HERE/usr/bin/nightslashers" "$@"
RUN
chmod +x "$APPDIR/AppRun"
ARCH=x86_64 "$TOOL" --appimage-extract-and-run --no-appstream "$APPDIR" "$ROOT/build/NightSlashers-x86_64.AppImage"
echo "built $ROOT/build/NightSlashers-x86_64.AppImage"
