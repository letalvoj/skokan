// Desktop harness: runs the real game core against a fake clock and a virtual
// panel, autopilots the runner, and dumps frames as BMPs for visual review.
//
//   ./host/build.sh && ./host/skokan [frames] [seed]
//
// Frames to capture are listed in SHOTS below (frame number -> filename).
#include <Adafruit_SPITFT.h>
#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include "../src/display.h"

void setup();
void loop();

int      gameProbe(float dx);
int      gameCheckWorld();
int      gameEggKind();
bool     gameEggOnScreen();
bool     gameGrounded();
int      gameState();
uint8_t  gameLevel();
uint32_t gameScore();

bool hostWriteBMP(const char *path, const uint16_t *fb, int w, int h, int scale);

static const uint32_t FRAME_MS = 45;    // ~22 fps, the rate the board should hit
static const int      SCALE    = 3;

static uint32_t frameNo   = 0;
static uint32_t holdUntil = 0;          // keep the button down until this ms
static uint32_t shotCount = 0;

struct Shot { uint32_t frame; const char *name; };
static Shot   shots[24];
static int    nshots = 0;
static char   outDir[256] = "host/shots";

// ---------------------------------------------------------------------------
// Autopilot. Looks a short way ahead -- the same information a player reading
// the screen has -- and holds the button for a full jump when something is
// coming. Deliberately not perfect: it should look like someone playing.
// ---------------------------------------------------------------------------
static void autopilot() {
  const int st = gameState();
  if (st != 1) {                                  // title / game over
    // Held for several frames: a single-frame tap is shorter than the
    // firmware's 15 ms debounce window and would never register.
    const uint32_t ph = frameNo % 40;
    hostButtonLevel = (ph >= 10 && ph < 14) ? LOW : HIGH;
    return;
  }
  if (hostMillis < holdUntil) { hostButtonLevel = LOW; return; }

  if (gameGrounded()) {
    // Scan the strip just ahead of the runner for anything worth clearing.
    for (float dx = 10.0f; dx < 74.0f; dx += 8.0f) {
      const int p = gameProbe(dx);
      if (p != 0) {
        holdUntil = hostMillis + (p == 1 ? 260 : 190);
        hostButtonLevel = LOW;
        return;
      }
    }
  }
  hostButtonLevel = HIGH;
}

static void capture(const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.bmp", outDir, name);
  if (hostWriteBMP(path, tft->pixels(), 320, 240, SCALE)) {
    printf("[shot] %s  (frame %u, level %u, score %u)\n",
           path, frameNo, gameLevel(), gameScore());
    shotCount++;
  } else printf("[shot] FAILED to write %s\n", path);
}

// Called by the game at the end of every rendered frame.
void hostFrameDone() {
  for (int i = 0; i < nshots; i++)
    if (shots[i].frame == frameNo) capture(shots[i].name);

  // Screens that happen when they happen: grab the first one of each rather
  // than guessing a frame number that lands on it.
  static bool gotOver = false, gotLevel = false;
  static int  overAge = 0;
  if (gameState() == 2) {                 // ST_OVER: wait for the blinking hint
    if (!gotOver && ++overAge > 30) { capture("07-gameover"); gotOver = true; }
  } else overAge = 0;

  static uint8_t lastLevel = 1;
  if (!gotLevel && gameLevel() == 3 && lastLevel == 2) { capture("08-levelup"); gotLevel = true; }
  lastLevel = gameLevel();

  // Catch one of each sky easter egg, mid-flight and well inside the frame.
  static bool gotEgg[4] = { false, false, false, false };
  static const char *eggName[4] = { "09-egg-plane", "09-egg-satellite",
                                    "09-egg-ufo", "09-egg-star" };
  const int ek = gameEggKind();
  if (ek >= 0 && ek < 4 && !gotEgg[ek] && gameEggOnScreen()) {
    capture(eggName[ek]); gotEgg[ek] = true;
  }
}

int main(int argc, char **argv) {
  const uint32_t total = (argc > 1) ? (uint32_t)atoi(argv[1]) : 1400;
  const unsigned seed  = (argc > 2) ? (unsigned)atoi(argv[2]) : 7;
  if (argc > 3) snprintf(outDir, sizeof(outDir), "%s", argv[3]);

  // Spread the captures: title, early easy running, then deeper levels where
  // crates and birds exist.
  shots[nshots++] = { 6,    "01-title" };
  shots[nshots++] = { 150,  "02-level1" };
  shots[nshots++] = { 430,  "03-level2" };
  shots[nshots++] = { 700,  "04-level3" };
  shots[nshots++] = { 1000, "05-level4" };
  shots[nshots++] = { 1300, "06-late" };

  srand(seed);
  setup();

  uint32_t badFrames = 0, worstBad = 0;
  for (frameNo = 0; frameNo < total; frameNo++) {
    autopilot();
    loop();
    // The fairness contract, checked on every single generated world state.
    const int bad = gameCheckWorld();
    if (bad > 0) {
      if (badFrames == 0)
        printf("[CHECK] FIRST VIOLATION at frame %u (level %u): %d\n",
               frameNo, gameLevel(), bad);
      badFrames++;
      if ((uint32_t)bad > worstBad) worstBad = bad;
    }
    hostMillis += FRAME_MS;
  }

  printf("[host] %u frames simulated (%.1f s of play), %u shots written\n",
         total, total * FRAME_MS / 1000.0, shotCount);
  printf("[CHECK] unclearable configurations: %u frames of %u (worst %u)  --> %s\n",
         badFrames, total, worstBad, badFrames ? "FAIL" : "PASS");
  return badFrames ? 1 : 0;
}
