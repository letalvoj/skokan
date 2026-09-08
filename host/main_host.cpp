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
static int realMain(int argc, char **argv);

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

// ---------------------------------------------------------------------------
// Measured rollouts. Headless (no rendering) so a few hundred games of play
// simulate in a second or two, and the difficulty of each level can be read
// off as numbers rather than argued about.
// ---------------------------------------------------------------------------
extern bool gHeadless;
extern bool hostQuiet;
void        gameSetBot(int profile, bool drive);
const char *gameBotName(int profile);
void        gameStatsReset();
void        gameStatsEndRun();
void        gameStartNow();
uint32_t    gameStatFrames(int lv), gameStatJumps(int lv), gameStatCoins(int lv);
uint32_t    gameStatDeath(int lv, int c), gameStatRuns(), gameStatLevelHist(int lv);
int         gameStatLevels();
float       gameDistance();

// Runs that got at least this far: a reverse cumulative of the max-level
// histogram, which is the survival curve.
static uint32_t survivors(int lv, int runs) {
  uint32_t n = 0;
  for (int k = lv; k < gameStatLevels(); k++) n += gameStatLevelHist(k);
  return n;
}

static void runStats(int profile, int runs, unsigned seed) {
  gHeadless = true;
  hostQuiet = true;          // the game logs per-frame; keep the table readable
  srand(seed);
  setup();
  gameStatsReset();

  double totalDist = 0.0;
  for (int r = 0; r < runs; r++) {
    gameStartNow();          // newGame() clears botDrive, so arm the bot AFTER
    gameSetBot(profile, true);
    for (int guard = 0; guard < 40000 && gameState() == 1; guard++) {
      loop();
      hostMillis += FRAME_MS;
    }
    totalDist += gameDistance();
    gameStatsEndRun();
    // Clear the game-over screen so the next run starts fresh.
    for (int i = 0; i < 60; i++) { loop(); hostMillis += FRAME_MS; }
  }

  const int L = gameStatLevels();
  printf("\n=== bot \"%s\"  %d runs ===\n", gameBotName(profile), runs);
  printf("  mean distance %.0f m\n", totalDist / runs / 32.0);
  printf("  lv   secs/run   deaths/min   water  crate   bird   jumps/s  coins/min  alive%%\n");
  for (int lv = 1; lv < L; lv++) {
    const uint32_t f = gameStatFrames(lv);
    if (!f) continue;
    const double secs = f * (FRAME_MS / 1000.0);
    const uint32_t d0 = gameStatDeath(lv, 0), d1 = gameStatDeath(lv, 1), d2 = gameStatDeath(lv, 2);
    const uint32_t dd = d0 + d1 + d2;
    printf("  %2d  %8.1f   %10.2f  %5u  %5u  %5u   %7.2f  %9.1f  %5u%%\n",
           lv, secs / runs, dd / (secs / 60.0), d0, d1, d2,
           gameStatJumps(lv) / secs, gameStatCoins(lv) / (secs / 60.0),
           (unsigned)(100.0 * survivors(lv, runs) / runs));
  }
}

// Never touch the button: the menu should time out and the bot take over.
static int runAttract(unsigned seed) {
  srand(seed);
  setup();
  hostButtonLevel = HIGH;
  for (frameNo = 0; frameNo < 700; frameNo++) {
    loop();
    if (frameNo == 60)  capture("10-menu-idle");
    if (frameNo == 220) capture("11-attract-demo");
    if (frameNo == 480) capture("12-attract-later");
    hostMillis += FRAME_MS;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--attract") == 0)
    return runAttract(argc > 2 ? (unsigned)atoi(argv[2]) : 4);
  if (argc > 1 && strcmp(argv[1], "--stats") == 0) {
    const int runs = (argc > 2) ? atoi(argv[2]) : 150;
    const unsigned seed = (argc > 3) ? (unsigned)atoi(argv[3]) : 1;
    for (int p = 0; p < 3; p++) runStats(p, runs, seed + p * 977);
    return 0;
  }
  return realMain(argc, argv);
}

static int realMain(int argc, char **argv) {
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
