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
