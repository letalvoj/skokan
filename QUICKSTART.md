# Quickstart

## 1. Install PlatformIO

```bash
uv tool install platformio
```

This puts `pio` on your PATH via `~/.local/bin/pio` (make sure that's in your
shell's `PATH`, or call it by full path as below).

## 2. Find the board

Plug the board in over the **CH343 USB-serial port** (not the other USB-C —
see [README](README.md#hardware) for why there are two). On macOS:

```bash
ls /dev/cu.usbmodem*
```

**The two ports enumerate under different names**, and the name changes
between sessions, so don't trust a hard-coded one:

- CH343 bridge (`1a86:55d3`) — e.g. `/dev/cu.usbmodem5C380098851`. This is
  the one `Serial` comes out of in every build except `game_usb`.
- Native USB (`303a:1001`, "USB JTAG/serial debug unit") — e.g.
  `/dev/cu.usbmodem101`. Flashing works here too, but you get **no console**
  unless you flash `game_usb`.

Check which one you have with `system_profiler SPUSBDataType | grep -i -B2
"303a\|1a86"`. `tools/monitor.py` auto-detects (override with
`MLUVITKO_PORT=/dev/cu.usbmodemXXX`); for flashing, pass
`--upload-port /dev/cu.usbmodemXXX` if PlatformIO picks the wrong one.

Sanity-check the board is alive:

```bash
uvx --from esptool esptool.py --port /dev/cu.usbmodem5C380098851 chip_id
```

Should report an ESP32-S3.

## 3. Build and flash

```bash
cd esp32-mluvitko
~/.local/bin/pio run -t upload
```

This builds and flashes the **default env (`mluvitko`)**, which is the full
current app — see [README.md](README.md#app-architecture) for what that
means and for the `step1`/`step2`/`step3` bring-up builds
(`pio run -e step2 -t upload`, etc.) if you're debugging hardware rather than
running the app.

## 4. Watch the serial output

`pio device monitor` needs an interactive terminal and won't work from a
script or from an AI agent's shell — this repo ships a drop-in replacement:

```bash
./tools/monitor.py 15                # watch for 15 seconds, resets the board first
./tools/monitor.py 8 --no-reset      # watch without resetting
./tools/monitor.py 6 --send r        # send the 'r' key, then watch
./tools/monitor.py 6 --send "di"     # send 'd' then 'i'
```

Useful runtime keys are listed in
[README.md#serial-commands-while-running](README.md#serial-commands-while-running)
— e.g. if the display looks wrong after wiring changes, cycle `d`/`r`/`i`
live and save with `s`, no reflash needed.

## 5. Try it

Press the arcade button. It should light up, play a jingle, and the face
should change mood. Press again to start "listening" (LED on, spectrum
analyser appears) — make some noise near the mic.

## 6. Play the game

```bash
~/.local/bin/pio run -e game -t upload
```

This flashes **SKOKAN**, the one-button platform runner. Turn the screen
landscape (or press `r` over serial to flip it the other way up) and press
the arcade button: tap for a hop, hold for a big jump, press again in mid-air
for a double jump. Full rules and the terrain generator are in
[README.md#skokan-the-game](README.md#skokan-the-game).

Go back to the talking box with `~/.local/bin/pio run -t upload`.

If something looks or sounds wrong, check
[README.md#learnings](README.md#learnings) first — most of the surprising
behaviour already has a documented cause and fix there.

## Common gotchas

- **`pio device monitor` hangs / errors about stdin** — use
  `tools/monitor.py` instead (see above).
- **Display looks mirrored or wrong colours** — try `d` (swap driver) or `i`
  (toggle inversion) live over serial, then `s` to save. See
  [README.md#learnings](README.md#learnings) for why this panel needs it.
- **Audio hisses or clicks** — check `GAIN` and `SD` on the amp are wired as
  described in [README.md](README.md#wiring), not left floating.
- **Mic reads all zeros** — check `L/R` actually reaches ground. The
  firmware auto-detects the active I²S slot and logs which one it found
  (`[mic] data is in the LEFT/RIGHT slot...`), which tells you if the wiring
  is backwards even though it still (degradedly) works.
