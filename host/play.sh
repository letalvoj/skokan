#!/usr/bin/env bash
# Build and run SKOKAN as a native macOS window. Same src/game.cpp the board
# runs; host/ supplies the screen, the clock, the button and the audio.
set -euo pipefail
cd "$(dirname "$0")/.."

GFX=".pio/libdeps/game/Adafruit GFX Library"
[ -d "$GFX" ] || GFX=".pio/libdeps/mluvitko/Adafruit GFX Library"
if [ ! -d "$GFX" ]; then
  echo "Adafruit GFX sources not found -- run 'pio run -e game' once first." >&2
  exit 1
fi
command -v sdl2-config >/dev/null || { echo "SDL2 not found: brew install sdl2" >&2; exit 1; }

clang++ -std=c++17 -O2 -DGAME_HOST -DARDUINO=200 \
  -Ihost/shim -I"$GFX" -Isrc \
  -Wno-unused-value -Wno-narrowing \
  $(sdl2-config --cflags) \
  src/game.cpp \
  host/play.cpp host/panel.cpp host/display_host.cpp host/synth_sdl.cpp \
  "$GFX/Adafruit_GFX.cpp" \
  $(sdl2-config --libs) \
  -o host/skokan-play

echo "built host/skokan-play"
[ "${1:-}" = "--build-only" ] || exec ./host/skokan-play
