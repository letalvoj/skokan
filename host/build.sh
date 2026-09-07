#!/usr/bin/env bash
# Build the macOS harness around the *unmodified* game core in src/game.cpp.
# See host/README.md for why this exists.
set -euo pipefail
cd "$(dirname "$0")/.."

GFX=".pio/libdeps/game/Adafruit GFX Library"
[ -d "$GFX" ] || GFX=".pio/libdeps/mluvitko/Adafruit GFX Library"
if [ ! -d "$GFX" ]; then
  echo "Adafruit GFX sources not found -- run 'pio run -e game' once first." >&2
  exit 1
fi

mkdir -p host/shots

# host/shim must come first so its Arduino.h / Adafruit_SPITFT.h win over any
# real ones, and -DGAME_HOST enables the harness hooks at the end of game.cpp.
clang++ -std=c++17 -O1 -g -DGAME_HOST -DARDUINO=200 \
  -Ihost/shim -I"$GFX" -Isrc \
  -Wno-unused-value -Wno-narrowing \
  src/game.cpp \
  host/main_host.cpp host/panel.cpp host/display_host.cpp host/synth_host.cpp \
  "$GFX/Adafruit_GFX.cpp" \
  -o host/skokan

echo "built host/skokan"
