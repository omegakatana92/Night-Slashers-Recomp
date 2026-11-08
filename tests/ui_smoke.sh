#!/bin/bash
# Runs the SDL2 front end under Xvfb with a scripted session and writes screenshots.
# usage: tests/ui_smoke.sh <nightslashers binary> <nslasher.zip> <out dir>
set -e
BIN=$1; ROM=$2; OUT=$3
mkdir -p "$OUT"; rm -rf "$OUT/data"
sed "s#SHOTS#$OUT#g" "$(dirname "$0")/ui_smoke.txt" > "$OUT/script.txt"
export SDL_AUDIODRIVER=dummy NSL_DATA_DIR="$OUT/data" NSL_UI_SCRIPT="$OUT/script.txt"
xvfb-run -a -s "-screen 0 1280x800x24" "$BIN" --rom "$ROM" > "$OUT/stdout.txt" 2>&1
echo "exit code $?"
