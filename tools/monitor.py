#!/usr/bin/env -S uv run --quiet --with pyserial python
"""Print the board's serial output for N seconds, then exit.

Unlike `pio device monitor` this needs no interactive terminal, so it works
from scripts. It can also send calibration keystrokes.

  tools/monitor.py 15                 # watch for 15s (resets the board first)
  tools/monitor.py 8 --no-reset       # watch without rebooting it
  tools/monitor.py 6 --send r         # press 'r' (next rotation), then watch
  tools/monitor.py 6 --send di        # 'd' then 'i'
"""
import os, sys, time, serial

# The board has two USB-C ports and they enumerate under different names, so
# auto-detect rather than hard-coding one. Override with MLUVITKO_PORT.
def _find_port():
    import glob
    if os.environ.get("MLUVITKO_PORT"):
        return os.environ["MLUVITKO_PORT"]
    ports = sorted(p for p in glob.glob("/dev/cu.usbmodem*")
                   if "debug-console" not in p)
    if not ports:
        sys.exit("No /dev/cu.usbmodem* found -- is the board plugged in?")
    return ports[0]


PORT = _find_port()
BAUD = 115200

args  = sys.argv[1:]
secs  = float(args[0]) if args and not args[0].startswith("-") else 15.0
reset = "--no-reset" not in args
keys  = args[args.index("--send") + 1] if "--send" in args else ""

with serial.Serial(PORT, BAUD, timeout=0.2) as s:
    if reset:
        # The usual auto-reset wiggle: DTR/RTS drive EN and IO0.
        s.setDTR(False); s.setRTS(True); time.sleep(0.1)
        s.setRTS(False); time.sleep(0.05)
        s.reset_input_buffer()
    if keys:
        time.sleep(1.5)                      # let it finish booting first
        for k in keys:
            s.write(k.encode()); s.flush()
            print(f"--- sent {k!r} ---")
            time.sleep(0.4)
    end = time.time() + secs
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
print(f"\n--- {secs:g}s elapsed ---")
