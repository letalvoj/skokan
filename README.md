# mluvitko

A talking box built on an ESP32-S3, together with an 8-year-old co-designer.
Press the arcade button: it lights up, sings a little chiptune jingle, and
its face reacts. Press again while it's "listening" and it shows a live
80s-radio-style spectrum of whatever's making noise nearby.

## Hardware

| Part | Notes |
|---|---|
| **ESP32-S3-N16R8** dev board | 16 MB flash, 8 MB **octal** PSRAM. The octal PSRAM occupies GPIO 26–37 — those pins are unusable for anything else. |
| **2.8" 240×320 SPI TFT**, silkscreened `GMT028-05 V1.1` | No MISO pin, so the controller can't be read back. Despite the panel's own datasheet naming ST7789, **this particular module is an ILI9341** (confirmed by eye — see [Learnings](#learnings)). |
| **I²S MEMS microphone** (INMP441-class, round 6-pin breakout) | Digital I²S, not analog. |
| **MAX98357A** I²S class-D amplifier | Drives a small speaker from the green screw terminal. |
| **60 mm arcade button** with backlit LED | The LED lamp is 12 V-rated with an internal 680 Ω resistor — see below. |

The board enumerates as two USB-C ports:
- One goes to a **CH343 USB-serial bridge** (`1a86:55d3`) — this is the port
  you flash and monitor through. Its DTR/RTS lines drive reset and boot, so
  flashing needs no manual button-holding.
- The other is the ESP32-S3's own **native USB** peripheral, wired directly
  to the chip (no bridge). Not used by this project yet, but the chip could
  enumerate as USB-MIDI, mass storage, etc. through it later.

### Wiring

Everything lives on the **3V3 side** of the dev board — that one row exposes
`3V3, 3V3, RST, 4, 5, 6, 7, 15, 16, 17, 18, 8, 3, 46, 9, 10, 11, 12, 13, 14, GND`,
which is 3.3 V, ground, and every GPIO this project needs. The whole build
fits on one breadboard row without jumping across the chip.

The dev board's `5Vin` pin does **not** carry USB VBUS on the CH343 port used
for flashing — it read no voltage at all. So the amp runs from 3.3 V instead
of 5 V (quieter max volume, ~0.5 W vs ~1 W into 4 Ω — plenty loud for a desk).

Bridge the rails first, since there are more grounds needed than the board
has pins for:
- board `3V3` → breadboard **red** rail
- board `GND` → breadboard **blue** rail

| Module | Pin | → |
|---|---|---|
| **TFT** | VCC | red rail (3.3 V) |
| | GND | blue rail |
| | CS | GPIO 10 |
| | SDA (MOSI) | GPIO 11 |
| | SCK | GPIO 12 |
| | DC | GPIO 13 |
| | RST | GPIO 14 |
| **Mic** | VDD | red rail |
| | GND | blue rail |
| | L/R | blue rail (selects the left I²S slot — see [Learnings](#learnings)) |
| | SCK (BCLK) | GPIO 4 |
| | WS | GPIO 5 |
| | SD | GPIO 6 |
| **Amp** | VIN | red rail (3.3 V) |
| | GND | blue rail |
| | **GAIN** | **blue rail — must be grounded, not left floating** (see below) |
| | SD | GPIO 7 (used as a software mute line, *not* left floating) |
| | BCLK | GPIO 15 |
| | LRC | GPIO 16 |
| | DIN | GPIO 17 |
| **Arcade button** | switch (COM + NO) | GPIO 8 → blue rail (uses `INPUT_PULLUP`) |
| | LED anode | via **47 Ω** resistor → GPIO 9 |
| | LED cathode | blue rail |

Speaker: two wires into the amp's green screw terminal; polarity doesn't
matter for a single speaker.

Avoid GPIO 0, 3, 45, 46 (strapping pins — can prevent boot if loaded), 19/20
(native USB), 26–37 (PSRAM), and 48 (onboard RGB LED, unused here but handy
later).

**The button LED.** It arrived as a 12 V lamp with a 680 Ω resistor built in
(blue-grey-brown-gold bands) — at 3.3 V that's under a milliamp, "barely
shining." The 680 Ω was desoldered and replaced with **47 Ω**, so the LED can
be driven straight off a GPIO through PWM (breathing effect) instead of
needing a boost converter or a 5 V rail that this board doesn't actually have
available.

## Learnings

Things that cost real debugging time, kept here so nobody has to rediscover
them:

- **The display is an ILI9341, not an ST7789**, despite the GMT028-05
  datasheet. Driving it as ST7789 renders mirrored, with red/blue-shifted
  colours (wrong MADCTL mirror bit, plus an `INVON` the panel doesn't want).
  Since the module has no MISO, the controller can't be probed — `display.cpp`
  keeps driver / rotation / inversion switchable live over serial and saves
  the working combination to flash (NVS), so recalibrating never needs a
  reflash. See [Serial commands](#serial-commands-while-running).

- **40 MHz SPI was too much for breadboard jumpers.** Caused white flashes
  and line-noise corruption on the display, worse under load. Dropped to
  **20 MHz** (`SPI_HZ` in `display.cpp`) and it's been clean since.

- **The MAX98357A `GAIN` pin must not float.** Left unconnected (a legal
  "9 dB" setting per the datasheet), it acted as an antenna next to the fast
  SPI/I²S lines and picked up an audible hiss. Grounding it (12 dB, plus a
  defined DC level) silenced it immediately. Lesson: on a breadboard, a
  "leave floating" datasheet option is a liability even when it's spec-legal.

- **The I²S DMA does not go silent when you stop feeding it.** With
  `dma_buf_count=8, dma_buf_len=256` at 22050 Hz, the driver holds ~93 ms of
  audio and **re-transmits the last buffer forever** once you stop writing.
  Symptom: the last note of a jingle looping ("e e e e e e...") until an
  unrelated timeout happened to mute the amp. Fix: after the last note, push
  more silent frames than the buffer depth (`DMA_DRAIN_MS = 200`) before
  doing anything else — merely *waiting* doesn't drain it, only writing does.
  This also fixed a crack heard at the *start* of playback: stale buffer
  contents were still queued when the amp woke up, so the fix is to zero the
  DMA buffer *before* enabling the amp, not after.

- **The MAX98357A needs ~250 ms after `SD` goes high before real audio
  sounds clean.** It's sampling `SD` to pick channel mode and running
  click/pop suppression. 60 ms was audibly bad; 250 ms is clean. The amp is
  kept awake for a few seconds after the last sound (`AMP_IDLE_OFF_MS`) so
  repeated button presses don't each pay that latency — only the first one
  after a period of silence does.

- **The mic's `L/R` pin decides which I²S stereo slot it speaks in.** If
  that wire isn't solidly grounded, the mic can silently speak in the
  *other* slot, and a receiver hard-wired to read only "left" sees nothing
  but zeros forever — while still measurably working (confirmed by touching
  the pin, which flips it and produces saturated bars). Fix in `mic.cpp`:
  read **both** stereo slots and auto-detect which one is carrying energy on
  each `micStart()`, so a wiring accident degrades gracefully instead of
  going silent. The detected slot is logged (`[mic] data is in the LEFT/RIGHT
  slot...`) so a real wiring fault is still visible.

- **A fixed dB threshold for "sound" doesn't work across rooms.** Office AC
  hum and fluorescent buzz sit well above any reasonable fixed floor, so
  speech had nowhere left to register. Fixed with a **per-band, self-learning
  noise floor** (rises slowly, falls fast) in `mic.cpp` — each of the 16
  spectrum bars learns its own quiet level and shows only what's above it
  plus a margin. Tunable live over serial (`+`/`-` margin, `[`/`]` span) with
  no reflash needed.

- **The arcade button's LED lamp resistor was found by reading the colour
  bands**: blue-grey-brown-gold = 6, 8, ×10, ±5% = 680 Ω (read right-to-left,
  gold/tolerance last).

## App architecture

The firmware is staged as three progressively-complete builds, kept because
they were essential for isolating hardware bugs (see Learnings above) and
remain useful if something regresses:

| PlatformIO env | What it runs | Use it to test |
|---|---|---|
| `step1` | button + speaker only, no display/mic compiled in at all | amp/speaker/power issues in isolation |
| `step2` | + display, still no mic | display + audio together, no I²S input conflict possible |
| `step3` | everything — this **is** the current app | the real thing |
| `mluvitko` (default) | same as `step3` | `pio run -t upload` with no `-e` flag |

### Source layout

- **`src/pins.h`** — every GPIO assignment, one place to check or change.
- **`src/display.h`/`.cpp`** — owns the SPI bus and the ST7789/ILI9341
  drivers. Runtime-switchable driver/rotation/inversion, persisted to flash.
  Exposes a generic `Adafruit_GFX *gfx` so drawing code doesn't care which
  controller is live.
- **`src/synth.h`/`.cpp`** — the chiptune voice. Owns I²S output + the amp's
  `SD` (mute) pin, runs on **core 1** in its own FreeRTOS task so a jingle
  plays without blocking the display loop on core 0. Three built-in jingles
  (power-up / ta-daa / ode to joy). Encapsulates every amp-timing and
  DMA-drain fix above so any future build gets them for free.
- **`src/mic.h`/`.cpp`** — the microphone + FFT. Owns I²S input. The
  peripheral is fully **stopped** (`i2s_stop`) while not listening, not just
  muted, so its clock lines sit genuinely idle next to the display's SPI.
  Auto-detects the live stereo slot and self-calibrates the noise floor on
  every `micStart()`.
- **`src/step3_full.cpp`** — the actual app: face, button handling, layout,
  and the 80s-style segmented spectrum analyser. This is what "mluvitko" is
  right now; read it top to bottom to see the whole behaviour.
- **`src/step1_button_speaker.cpp`**, **`src/step2_display_button_speaker.cpp`**
  — the earlier bring-up stages, kept for regression testing (see table
  above). `step1` additionally has extra diagnostic serial commands
  (`z`/`t`/`a`/`l`/`w`) used while chasing the audio bugs above.

### What it currently does

Press the arcade button:
1. The **LED toggles** — this is also the "recording" indicator. On: PWM
   breathing glow; off: dark.
2. **When the LED turns on**, the mic starts and a **16-bar, 8-segment
   log-spaced spectrum analyser** appears under the face — green/yellow/red
   LED-style segments with slowly-falling white peak-hold caps, in the style
   of an old radio/hi-fi VU meter. Log-spaced 80 Hz–6 kHz so speech fills the
   display instead of hiding in the first two bars.
3. **Every press** also plays one of three random chiptune jingles through
   the speaker, and cycles the face's mood: happy → wink → cool (sunglasses)
   → surprised.
4. While a jingle plays, **the face sings**: mouth width tracks the current
   note's pitch (higher note = wider mouth), and the note name (e.g. `C5`)
   is printed under the face. When nothing is playing but the mic is
   listening, the mouth instead tracks room loudness — it "talks back."
5. When idle (not singing, not listening), the face **blinks** at
   irregular intervals so it reads as alive rather than static.

### Serial commands while running

At 115200 baud, type single characters into the serial monitor:

| Key | Effect |
|---|---|
| `d` | swap display driver (ST7789 ↔ ILI9341) |
| `r` | next display rotation (0–3) |
| `i` | toggle display colour inversion |
| `s` | **save** the current display driver/rotation/inversion to flash |
| `?` | print current display settings |
| `+` / `-` | raise/lower the mic's noise-floor margin (how loud before a band lights) |
| `[` / `]` | shrink/grow the mic's dB span (how much shouting fills the display) |

`step1` (button+speaker bring-up build) additionally has: `1`/`2`/`3` play a
specific jingle, `space` replay the last one, `w` cycle waveform
(sine/triangle/square), `a` keep the amp always-on (diagnostic), `l` disable
the LED's PWM (diagnostic), `z` play 3 s of pure digital silence, `t` play a
steady 440 Hz tone — the last two were what isolated the amp/power bugs
above from the DMA-drain bug.

## Where to go next

Ideas discussed but not yet built:
- A note-matching game (sing the target pitch, score by cents off).
- "Parrot mode": hold the button to record, release to play back
  slowed/sped/reversed.
- A loudness high-score / screaming contest.
- Long-press to switch modes instead of cycling moods every press.
- Driving the onboard RGB LED (GPIO 48, currently unused) as a colour organ.

See [QUICKSTART.md](QUICKSTART.md) for how to build, flash, and monitor.
