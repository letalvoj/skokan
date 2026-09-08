# Desktop harness

Runs **the real game core** (`src/game.cpp`, unmodified) as a macOS binary, so
the look and feel of SKOKAN can be worked on without a board on the desk — and
so a change can be *seen* rather than guessed at.

```bash
./host/shots.sh              # build, simulate a run, write host/shots/*.png
./host/shots.sh 2500 42      # 2500 frames, RNG seed 42
```

## How it works

- `shim/` is just enough Arduino to compile against: `Arduino.h`, `Print.h`,
  `Preferences.h`, `esp_heap_caps.h`, and empty stubs for the BusIO headers
  `Adafruit_GFX.h` insists on including. `-Ihost/shim` comes first on the
  command line so these win over anything real.
- **The graphics are not simulated.** The harness compiles the actual
  `Adafruit_GFX.cpp` from `.pio/libdeps`, so `GFXcanvas16`, the 5x7 font and
  every `fillRoundRect` behave exactly as they do on the board. Only the final
  SPI push is replaced — `shim/Adafruit_SPITFT.h` is an `Adafruit_GFX` whose
  pixels land in a plain RGB565 buffer. It deliberately keeps the real class
  name *and* reproduces the fact that its `drawRGBBitmap` hides the
  non-virtual base one, since that shadowing is the reason `display.h` exposes
  `tft` separately.
- **The clock is fake.** `millis()` reads a counter the harness advances by a
  fixed 45 ms per frame, so a minute of play simulates in a fraction of a
  second and runs are perfectly reproducible for a given seed. It also means
  the harness says nothing about the real frame rate — that number has to come
  off the board.
- `display_host.cpp` and `synth_host.cpp` implement the same headers the
  firmware uses; the synth is silent.
- An **autopilot** (`main_host.cpp`) plays the game, probing a short way ahead
  through the `GAME_HOST` hooks at the bottom of `game.cpp` and holding the
  button when something is coming. It plays about as well as a distracted
  adult, which is roughly the point — it exercises gaps, crates and birds.

- It also **checks the fairness contract on every frame**. `gameCheckWorld()`
  counts genuinely unclearable configurations — anything forcing a jump with a
  bird inside the arc that jump must travel, and birds over water — and the
  run exits non-zero if any are found, so it works as a regression test:

  ```bash
  ./host/skokan 12000 42 | grep CHECK
  ```

  It has been negative-tested: reinstating the old drifting birds makes it
  fail at frame 540. A check that has never failed proves nothing.

Captures are written when a listed frame comes up, plus automatically the
first time the game reaches the game-over screen, the first level-up banner,
and one of each of the four sky easter eggs mid-flight. `sips` converts them to PNG; the 320x240 frame is scaled 3x with
nearest-neighbour so the 2x2 pixel art stays crisp.

## What it is not

It renders the same pixels the panel gets, but it cannot tell you about SPI
timing, DMA, the amp's wake latency, button bounce, or whether the display's
colour order is right on the real module. Those still need the board.

## Measuring difficulty

```bash
./host/skokan --stats 400     # ~1200 games in half a second
./host/skokan --attract       # capture the arcade attract screens
```

`--stats` runs the firmware's own bot headless (rendering skipped) at three
skill profiles and prints, per level: seconds per run, deaths per minute split
by cause, jumps/s, coins/min, and a survival curve. Skill is one knob — timing
error — plus a rate of needless jumps, which are the two ways a small child
actually fails.

The bot is not a harness toy: it lives in `src/game.cpp` and is the same code
that plays the attract mode on the board. It drives the same virtual button a
child does, so it is bound by coyote time, jump buffering and hold-for-height.
A bot that bypassed the physics would measure nothing.

Read the table for *shape*, not absolutes: deaths/min should rise monotonically
with level, level 1–2 should be near zero, and a better profile should get
meaningfully further than a worse one. When that last property failed — ace
scoring the same as a masher — it turned out to be a physics bug, not a
balance problem.

## Playing it on macOS

```bash
make play          # build SKOKAN.app and launch it
```

The app compiles the **same `src/game.cpp`** the board runs, completely
unmodified. `host/` supplies only what the ESP32 normally would:

| on the board | on macOS |
|---|---|
| ILI9341 over SPI | an SDL texture fed the same 320x240 RGB565 framebuffer |
| `millis()` | the real clock |
| arcade button (pull-up) | SPACE, mapped to `hostButtonLevel` |
| MAX98357A over I2S | an SDL audio callback |

`host/synth_sdl.cpp` copies the oscillator and envelope from `src/synth.cpp`
on purpose: if the chiptune is going to be tuned by ear on a laptop, it has to
be the same voice that comes out of the speaker. None of the amp workarounds
(SD wake time, DMA drain) are there — those exist because of the MAX98357A,
and a sound card has no such problem.

Controls: **SPACE** (or up/W/mouse) is the button — tap to hop, hold for
height, tap again in mid-air to double jump. `1`/`2`/`3` hand the controls to
the 4yo/8yo/ace bot, `0` takes them back, `F` changes window size, `ESC`
quits. Leave the menu alone for five seconds and attract mode starts, exactly
as on the board.

The icon is **rendered by a program** (`host/icon.cpp`), not cropped from a
screenshot — a screenshot crop is unreadable at 32 px, all HUD and text. It
follows Apple's icon grid: a centred 824x824 rounded square on a 1024 canvas,
transparent outside, because a full-bleed square reads as broken next to every
other icon in the Dock.
