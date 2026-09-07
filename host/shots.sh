#!/usr/bin/env bash
# Build the harness, simulate a run, and convert the captured frames to PNG.
#   ./host/shots.sh [frames] [seed]
set -euo pipefail
cd "$(dirname "$0")/.."
./host/build.sh
./host/skokan "${1:-1400}" "${2:-7}" | grep -vE '^\[game\] [0-9]+\.[0-9]+ fps' || true
for f in host/shots/*.bmp; do
  sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null
done
rm -f host/shots/*.bmp
echo "--> host/shots/*.png"
