#!/usr/bin/env bash
# Snapshot the currently-built firmware images (elf/bin/map) for every env,
# labelled with git commit + timestamp, so "what's actually on the chip"
# has a local paper trail even though the binaries aren't in git history
# (they're fully reproducible from source -- see .gitignore).
set -euo pipefail
cd "$(dirname "$0")/.."

# Optional label, so a snapshot can say what it was rather than only when it
# was:  ./tools/backup_firmware.sh skokan-flashed
label="${1:-}"
commit=$(git rev-parse --short HEAD 2>/dev/null || echo "nogit")
dirty=$(git diff --quiet 2>/dev/null || echo "-dirty")
stamp=$(date +%Y%m%d-%H%M%S)
dest="firmware_backups/${stamp}_${commit}${dirty}${label:+_$label}"
mkdir -p "$dest"

found=0
for envdir in .pio/build/*/; do
  env=$(basename "$envdir")
  if [ -f "$envdir/firmware.bin" ]; then
    cp "$envdir/firmware.bin" "$dest/${env}.bin"
    cp "$envdir/firmware.elf" "$dest/${env}.elf" 2>/dev/null || true
    found=1
  fi
done

if [ "$found" = 0 ]; then
  echo "No built firmware found -- run 'pio run' first." >&2
  exit 1
fi

echo "Backed up to $dest:"
ls -la "$dest"
