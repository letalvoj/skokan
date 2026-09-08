// esp32-mluvitko -- SKOKAN ("jumper"): a one-button neon platform runner.
//
// The whole game is played with the arcade button, Mario style:
//   tap            -> short hop      (~26 px)
//   hold           -> full jump      (~66 px) -- gravity is lower while held
//   press in mid-air -> DOUBLE JUMP  (once per airtime, the "double click")
//   land on a bird -> stomp + bounce; three in one airtime = TRIPLE
// Everything else is automatic: the runner never stops running.
//
// The terrain is generated on the fly, but never generates something the
// player cannot clear -- see reachability() below, which derives the limits
// from the very same physics constants the player obeys.
//
// Rendering: the play field is composed off-screen into a 320x176 canvas and
// blitted in one bulk SPI transfer. That transfer is the frame budget (a full
// 320x240 screen is ~61 ms at 20 MHz, which is why the play field is a band
// and the HUD/apron around it are drawn only when they change).

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <math.h>
#include "display.h"
#include "synth.h"
#include "pins.h"

// ---------------------------------------------------------------------------
// Layout.  The panel runs in landscape: 320 wide, 240 tall.
// ---------------------------------------------------------------------------
static const int16_t SCR_W  = 320, SCR_H = 240;
static const int16_t HUD_H  = 22;                 // y 0..21     drawn on change
static const int16_t PLAY_Y = HUD_H;              // y 22..197   redrawn every frame
static const int16_t PLAY_W = 320, PLAY_H = 176;
static const int16_t APRON_Y = PLAY_Y + PLAY_H;   // y 198..239  drawn once

// Geometry inside the play band (band-local y). The horizon sits LOW, just
// above the ground line: a high horizon leaves a wide stripe of floor grid in
// the air above the platforms, and the runner then looks like it is standing
// on the grid rather than on the ground it actually collides with.
static const int16_t HORIZON = 134;
static const int16_t SUN_X = 244, SUN_Y = 56, SUN_R = 33;
static const int16_t TOP_MIN = 106, TOP_MAX = 148;   // platform surface range
// Water fills every gap, starting just below the lowest platform. Touching it
// is death -- drawing the danger instead of leaving an empty void is what
// makes the rule legible to a four-year-old.
static const int16_t WATER_Y = TOP_MAX + 6;          // 154

// ---------------------------------------------------------------------------
// Palette -- deliberately tiny, 80s arcade.
// ---------------------------------------------------------------------------
static const uint16_t C_NONE   = 0x0000;
static const uint16_t C_MAG    = 0xF81F;   // hot magenta
static const uint16_t C_CYAN   = 0x07FF;
static const uint16_t C_ORANGE = 0xFC00;
static const uint16_t C_YELLOW = 0xFFE0;
static const uint16_t C_WHITE  = 0xFFFF;
static const uint16_t C_DARK   = 0x1002;   // near-black violet
static const uint16_t C_GRID   = 0x30BF;   // dim blue-violet grid
static const uint16_t C_ROCK   = 0x2809;   // platform body
static const uint16_t C_MTN    = 0x5008;   // mountain silhouette
static const uint16_t C_BLACK  = 0x0000;

// Sprite palette indices (0 = transparent).
static const uint16_t PAL[8] = { C_NONE, C_MAG, C_CYAN, C_ORANGE,
                                 C_YELLOW, C_WHITE, C_ROCK, C_DARK };

// ---------------------------------------------------------------------------
// Sprites.  Authored at 8x10 / 8x5 and drawn at scale 2, so every game pixel
// is a 2x2 block on the panel -- that is where the chunky look comes from.
// ---------------------------------------------------------------------------
static const uint8_t SPR_RUN_A[10][8] = {
  {0,0,2,2,2,2,0,0},
  {0,2,2,2,2,2,2,0},
  {0,2,5,2,2,5,2,0},
  {0,0,2,2,2,2,0,0},
  {0,1,1,1,1,1,1,0},
  {1,1,1,1,1,1,1,1},
  {1,0,1,1,1,1,0,1},
  {0,0,1,1,1,1,0,0},
  {0,0,3,3,3,0,0,0},
  {0,3,3,0,0,3,3,0},
};
static const uint8_t SPR_RUN_B[10][8] = {
  {0,0,2,2,2,2,0,0},
  {0,2,2,2,2,2,2,0},
  {0,2,5,2,2,5,2,0},
  {0,0,2,2,2,2,0,0},
  {0,1,1,1,1,1,1,0},
  {1,1,1,1,1,1,1,1},
  {1,0,1,1,1,1,0,1},
  {0,0,1,1,1,1,0,0},
  {0,0,0,3,3,3,0,0},
  {0,3,3,0,0,3,3,0},
};
static const uint8_t SPR_JUMP[10][8] = {
  {0,0,2,2,2,2,0,0},
  {0,2,2,2,2,2,2,0},
  {0,2,5,2,2,5,2,0},
  {0,0,2,2,2,2,0,0},
  {1,1,1,1,1,1,1,1},
  {1,0,1,1,1,1,0,1},
  {0,0,1,1,1,1,0,0},
  {0,0,1,1,1,1,0,0},
  {0,3,3,0,0,3,3,0},
  {0,0,0,0,0,0,0,0},
};
// Birds are WHITE, not a dark silhouette: they cost a life, and on a dusk
// background a dark bird is invisible until it has already hit you.
static const uint8_t SPR_BIRD_A[5][8] = {
  {5,0,0,0,0,0,0,5},
  {0,5,5,0,0,5,5,0},
  {0,0,5,5,5,5,0,0},
  {0,0,5,7,5,5,5,0},
  {0,0,0,5,5,0,0,0},
};
static const uint8_t SPR_BIRD_B[5][8] = {
  {0,0,0,0,0,0,0,0},
  {0,0,5,5,5,5,0,0},
  {0,5,5,7,5,5,5,0},
  {0,5,0,5,5,0,5,0},
  {5,0,0,0,0,0,0,5},
};

static const int16_t PLR_W = 16, PLR_H = 20;   // on-screen size (art 8x10 @ 2x)
static const int16_t BIRD_W = 16, BIRD_H = 10;
static const int16_t CRATE_S = 18;
static const int16_t COIN_S  = 12;
static const int16_t PLAYER_X = 66;            // fixed screen x of the runner

// ---------------------------------------------------------------------------
// Physics, in pixels and seconds. Everything downstream (how wide a gap may
// be, how high a step may be, where coins go) is derived from these, so they
// are the only numbers to touch when tuning difficulty.
// ---------------------------------------------------------------------------
static const float JUMP_V0    = -335.0f;   // initial upward velocity
static const float DBL_V0     = -290.0f;   // the mid-air second jump
static const float G_UP_HELD  =  850.0f;   // rising, button still held
static const float G_UP_FREE  = 2100.0f;   // rising, button released -> hop
static const float G_DOWN     = 2300.0f;   // falling
static const float VY_MAX     =  760.0f;
static const float STOMP_V0   = -270.0f;

// Peak of a fully-held jump, and how long the whole arc lasts.
static const float JUMP_H  = (JUMP_V0 * JUMP_V0) / (2.0f * G_UP_HELD);           // ~66 px
static const float T_UP    = -JUMP_V0 / G_UP_HELD;                               // ~0.39 s
static const float T_DOWN  = 1.4142f * sqrtf(JUMP_H / G_DOWN);                    // ~0.24 s
static const float AIRTIME = T_UP + T_DOWN;                                       // ~0.63 s

// Speed ramp. Deliberately slow at the start -- a 4-year-old has to be able
// to see the gap coming before it arrives.
static const float SPEED_0    = 88.0f;     // px/s at level 1
static const float SPEED_STEP = 1.078f;    // per level, spread over DIFF_LEVELS
static const float SPEED_MAX  = 235.0f;
static const float LEVEL_DIST = 900.0f;    // px of ground per level

static const uint8_t START_LIVES = 3;
static const uint16_t COIN_POINTS  = 25;   // vs ~0.12 x speed points/s of distance
static const uint16_t GEM_POINTS   = 150;
static const uint8_t  MAX_LIVES    = 5;

// ---------------------------------------------------------------------------
// Pacing: the escape window.
//
// The one rule that makes the world fair is that hazards are SEPARATED. Every
// hazard -- a gap, a crate, a bird -- reserves clear flat ground after it
// before the next one may start, and that window is measured in SECONDS of
// reaction time rather than pixels: at 235 px/s the same pixel gap gives less
// than half the thinking time it does at 88, so pixels are the wrong unit for
// difficulty. Narrowing this window is the single knob that makes the game
// harder, and it narrows every level.
//
// SEP_T_MIN is load-bearing, not taste: it is comfortably longer than
// AIRTIME (0.63 s), which is what guarantees a jump can never carry you into
// the next hazard, at any speed. Do not drop it below AIRTIME.
static const float SEP_T0     = 1.80f;   // seconds of clear ground at level 1
static const float SEP_T_MIN  = 0.80f;   // floor -- see above

// Difficulty is one normalised ramp, 0 at level 1 and 1 at DIFF_LEVELS, and
// every knob reads it. Before this, speed capped at level 11 and the escape
// window floored at level 9, so from level 9 on the game stopped getting
// harder at all -- measured as a dead-flat 0.3 deaths/min from level 4 to 13.
static const uint8_t DIFF_LEVELS = 14;
static float diffT();          // defined once `level` exists, below

// Which hazards exist yet. One new skill at a time, so a four-year-old is
// never learning two things at once.
static const uint8_t LV_CRATES    = 2;
static const uint8_t LV_BIRDS_LOW = 3;
static const uint8_t LV_BIRDS_ALL = 4;

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------
struct Seg   { float x, w; int16_t top; };
// One struct for everything you can pick up, so culling, collision and
// drawing stay in one place. GEM is a rare 4x coin; HEART gives a life back.
enum PickKind : uint8_t { PK_COIN = 0, PK_GEM, PK_HEART };
struct Coin  { float x, y; bool alive; uint8_t kind; };
struct Crate { float x; int16_t top; };
struct Bird  { float x, y, phase; bool alive, scored; };
struct Part  { float x, y, vx, vy; uint8_t life, size; uint16_t col; };

static const uint8_t MAX_SEG = 14, MAX_COIN = 64, MAX_CRATE = 16;
static const uint8_t MAX_BIRD = 6, MAX_PART = 52;   // room for a proper explosion

static Seg   segs[MAX_SEG];    static uint8_t nseg;
static Coin  coins[MAX_COIN];  static uint8_t ncoin;
static Crate crates[MAX_CRATE];static uint8_t ncrate;
static Bird  birds[MAX_BIRD];  static uint8_t nbird;
static Part  parts[MAX_PART];

static float    camX;              // world x at the left edge of the screen
static float    speed;             // px/s
static uint8_t  level;
static float diffT() {
  return constrain((float)(level - 1) / (float)(DIFF_LEVELS - 1), 0.0f, 1.0f);
}
static uint32_t score, best;
static uint16_t coinsGot;
static uint8_t  lives;
static float    distance;          // px run this game
static float    scoreAcc;          // sub-point remainder of the distance score
// World x from which the next hazard may START. Every hazard placement reads
// it and pushes it forward, which is what keeps gaps, crates and birds from
// ever landing on top of each other -- there is exactly one authority.
static float    nextHazardX;

// Player
static float   py, vy;             // band-local y of the sprite top
static bool    grounded, usedDouble, holding;
static float   coyote, buffered, invuln;
static uint8_t birdCombo;

// ---- sky easter eggs ------------------------------------------------------
// About one per level, drifting across the upper sky. Purely decorative and
// never near the ground: the reward is spotting one, so they are deliberately
// quiet -- no sound, no banner, and only one at a time.
enum EggKind : uint8_t { EGG_PLANE = 0, EGG_SAT, EGG_UFO, EGG_STAR, EGG_KINDS };
struct SkyEgg { uint8_t kind; float x, y, vx, phase; bool active; };
static SkyEgg  egg;
static float   eggNextDist;      // spawn the next one when distance passes this

// ---- measurement (desktop harness only) -----------------------------------
// Per-level counters, so difficulty can be looked at as numbers instead of
// vibes. Compiled out of the firmware entirely.
#ifdef GAME_HOST
static const uint8_t STAT_LV = 22;
uint32_t statFrames[STAT_LV], statJumps[STAT_LV], statCoins[STAT_LV];
uint32_t statDeath[STAT_LV][3];      // 0 = water, 1 = crate, 2 = bird
uint32_t statRuns, statLevelHist[STAT_LV], statRunMax, statCoinSpawn[STAT_LV];
uint32_t statScore;
bool     gHeadless = false;          // skip rendering: rollouts run ~100x faster
#define STAT(arr)      do { if (level < STAT_LV) arr[level]++; } while (0)
#define STAT_DEATH(c)  do { if (level < STAT_LV) statDeath[level][c]++; } while (0)
#else
#define STAT(arr)      do {} while (0)
#define STAT_DEATH(c)  do {} while (0)
#endif

// ---- the bot ---------------------------------------------------------------
// Skill is one knob -- timing error -- plus a rate of needless jumps. Those
// are exactly the two ways a small child fails: mistimed take-offs, and
// pressing the button when nothing asked them to.
struct BotSkill {
  float   jitter;      // +/- seconds of error on the take-off point
  uint8_t sloppyPct;   // chance per second of a pointless jump
  const char *name;
};
static const BotSkill BOT_PROFILES[] = {
  { 0.090f, 26, "4yo" },     // mashy, poor timing
  { 0.045f,  8, "8yo" },     // decent, occasionally excited
  { 0.012f,  0, "ace" },     // near-perfect reference
};
static const uint8_t BOT_KINDS = 3;

static BotSkill botSkill   = BOT_PROFILES[1];
static bool     botDrive   = false;   // the bot has the controls
static bool     botHolding = false;
static float    botHoldT   = 0.0f;
static float    botSloppyT = 0.0f;

enum State : uint8_t { ST_TITLE, ST_PLAY, ST_OVER };
static State state = ST_TITLE;
static uint32_t stateSince;

static char     banner[20];
static uint32_t bannerUntil;

// ---------------------------------------------------------------------------
// Canvas.  GFXcanvas16 normally malloc()s; we want it in internal SRAM if it
// fits (faster to read back over the SPI blit) and PSRAM only as a fallback.
// ---------------------------------------------------------------------------
class Canvas : public GFXcanvas16 {
public:
  Canvas(uint16_t w, uint16_t h) : GFXcanvas16(w, h, false) {
    const size_t bytes = (size_t)w * h * 2;
    buffer = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer) Serial.printf("[game] canvas %ux%u (%u B) in internal SRAM\n", w, h, bytes);
    else {
      buffer = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
      Serial.printf("[game] canvas %ux%u (%u B) in %s\n", w, h, bytes,
                    buffer ? "PSRAM" : "NOWHERE -- allocation failed!");
    }
    buffer_owned = (buffer != nullptr);
  }
};
static Canvas *cv = nullptr;
// The HUD gets its own canvas for the same reason the play field has one:
// drawing it straight to the panel means clearing the bar to black and then
// putting text back, and at 17 fps that gap is visible as a flickering bar.
static Canvas *hudCv = nullptr;

static Preferences prefs;
static uint8_t  g_rot = 1;            // landscape; 'r' flips to the other one
static bool     g_needStatic = true;  // HUD + apron need a repaint
static uint32_t g_lastHud = 0xFFFFFFFF;

// Precomputed background
static uint16_t skyLUT[PLAY_H];
static uint8_t  hudDraws = 0;      // HUD repaints per 40 frames, for the log
// Ground shading, indexed by absolute y so neighbouring platforms at
// different heights share one consistent light, and the apron below the play
// band continues it seamlessly instead of looking like a separate surface.
static uint16_t rockLUT[PLAY_H];
static const uint16_t MTN_N = 512;
static uint8_t  mtn[MTN_N];

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}
static uint16_t lerpRGB(uint8_t r0, uint8_t g0, uint8_t b0,
                        uint8_t r1, uint8_t g1, uint8_t b1, float t) {
  return rgb((uint8_t)(r0 + (r1 - r0) * t),
             (uint8_t)(g0 + (g1 - g0) * t),
             (uint8_t)(b0 + (b1 - b0) * t));
}
static inline float frnd(float a, float b) {
  return a + (b - a) * (float)random(0, 10001) / 10000.0f;
}

static void blitSprite(int16_t x, int16_t y, const uint8_t *d,
                       uint8_t w, uint8_t h, uint8_t s) {
  for (uint8_t r = 0; r < h; r++)
    for (uint8_t c = 0; c < w; c++) {
      const uint8_t p = d[r * w + c];
      if (p) cv->fillRect(x + c * s, y + r * s, s, s, PAL[p]);
    }
}

// ---------------------------------------------------------------------------
// Reachability -- the "mostly always clearable" heuristic.
//
// A jump covers speed * AIRTIME horizontally. We only ever ask for a fraction
// of that (SAFETY), so a mistimed jump still lands; and a gap that also steps
// UP costs height, which eats into the horizontal reach, so the allowance
// shrinks proportionally to how much of the jump's height the step uses.
// Because both terms are computed from the live `speed`, the terrain scales
// itself as the game accelerates instead of needing a per-level table.
// ---------------------------------------------------------------------------
// How much of a full jump a gap is allowed to consume. It ramps too: level 1
// asks for less than half the runner's reach, the top of the curve asks for
// nearly three quarters. Never anywhere near 1.0 -- that would demand a
// perfectly timed, fully held jump every time.
static float safetyNow() { return 0.45f + 0.27f * diffT(); }

static float maxGapFor(float rise) {
  const float reach   = speed * AIRTIME * safetyNow();
  const float heightF = (rise > 0.0f) ? (1.0f - rise / JUMP_H) : 1.0f;
  return constrain(reach * heightF, 16.0f, 130.0f);
}
// The tallest step up we will ever generate: well inside the full jump, so it
// is clearable even without holding the button all the way.
static float maxRise() { return JUMP_H * 0.42f; }   // ~27 px

// ---------------------------------------------------------------------------
// Terrain generation
// ---------------------------------------------------------------------------
static void addCoin(float x, float y) {
  if (ncoin >= MAX_COIN) return;
  STAT(statCoinSpawn);
  coins[ncoin++] = { x, y, true, PK_COIN };
}

static void addPickup(float x, float y, uint8_t kind) {
  if (ncoin >= MAX_COIN) return;
  coins[ncoin++] = { x, y, true, kind };
}

// Lay coins along the actual jump parabola out of (x0, y0). Collecting them
// *is* the correct jump, which is how a 4-year-old learns the timing.
static void coinArc(float x0, float y0, uint8_t n) {
  for (uint8_t i = 1; i <= n; i++) {
    const float t = (float)i / (n + 1) * AIRTIME;
    const float dy = (t < T_UP) ? (JUMP_V0 * t + 0.5f * G_UP_HELD * t * t)
                                : (-JUMP_H + 0.5f * G_DOWN * (t - T_UP) * (t - T_UP));
    // Flattened to 80% of the true arc: a full held jump still sweeps through
    // them, but so does a merely decent one, which is the point at this age.
    addCoin(x0 + speed * t, y0 + dy * 0.80f - COIN_S);
  }
}

// A rhythm run: the same obstacle, the same spacing, two or three times over.
// Repetition is the thing a small child can actually learn from -- once the
// first one is cleared, the next two are the *same movement* on a beat, which
// is how timing gets into the hands. Endless novelty teaches nothing.
static uint8_t runLeft;
static float   runGap, runW;

// The escape window in pixels, at the current speed and level.
static float hazardSep() {
  const float t = SEP_T0 + (SEP_T_MIN - SEP_T0) * diffT();
  return speed * t;
}

// Place at most one crate or bird on this segment, and only in the part of it
// that is already clear of every other hazard. Everything goes through
// nextHazardX, which is why a bird can no longer end up sitting over a crate:
// the crate pushed the window past the bird's only legal position.
static void placeHazard(const Seg &s) {
  if (level < LV_CRATES) return;                 // level 1 is gaps only
  const float sep = hazardSep();
  const float lo  = fmaxf(s.x + 46.0f, nextHazardX);
  const float hi  = s.x + s.w - 54.0f;
  if (lo > hi) return;                           // no legal room on this one
  if (random(100) > 60 + (int)(24 * diffT())) return;   // density ramps too

  const float x = frnd(lo, hi);
  const bool  wantBird = (level >= LV_BIRDS_LOW) && (random(100) < 40 + (int)(22 * diffT()));

  if (wantBird && nbird < MAX_BIRD) {
    // LOW birds clear a standing runner's head by a few pixels, so they are
    // harmless if you keep running and only ever punish a jump you did not
    // need -- the inhibition lesson. The height accounts for the vertical
    // bob amplitude, or the bob alone could clip a standing player.
    const bool low = (level < LV_BIRDS_ALL) || (random(100) < 55);
    const float h  = low ? frnd(38.0f, 54.0f) : frnd(62.0f, 84.0f);
    birds[nbird++] = { x, (float)s.top - h, frnd(0, 6.28f), true, false };
    nextHazardX = x + BIRD_W + sep;
  } else if (ncrate < MAX_CRATE) {
    crates[ncrate++] = { x, (int16_t)(s.top - CRATE_S) };
    coinArc(x - 34.0f, (float)s.top, 3);
    nextHazardX = x + CRATE_S + sep;
  }
}

static void addSegment() {
  const Seg &p = segs[nseg - 1];
  const float sep     = hazardSep();
  const float prevEnd = p.x + p.w;      // where a gap here would begin
  float   gap = 0.0f, w;
  int16_t top = p.top;
  float   actualRise = 0.0f;

  // A gap is a hazard like any other: it may only open once the previous
  // hazard's escape window has been paid out.
  const bool gapAllowed = (prevEnd >= nextHazardX);

  if (runLeft > 0 && gapAllowed) {
    runLeft--;                          // mid-run: reproduce the beat exactly
    gap = runGap;
    w   = runW;
  } else {
    runLeft = 0;
    const float rise = (random(100) < 42) ? frnd(4.0f, maxRise()) : 0.0f;
    const float drop = (rise == 0.0f && random(100) < 40) ? frnd(6.0f, 40.0f) : 0.0f;
    top = (int16_t)constrain((float)p.top - rise + drop, (float)TOP_MIN, (float)TOP_MAX);
    actualRise = (float)(p.top - top);

    if (gapAllowed && random(100) < 66)
      gap = frnd(18.0f, maxGapFor(actualRise));

    // Segments are at least one escape window wide, so the NEXT gap is always
    // legally placeable -- otherwise gaps would starve at high level, where
    // the window is wide relative to a segment.
    w = sep + frnd(20.0f, 130.0f);

    // Turn this beat into a run. Never for a step that also rises: a repeated
    // flat gap is a rhythm, a repeated staircase is just a wall.
    if (level >= LV_CRATES && gap > 0.0f && actualRise <= 0.0f && random(100) < 38) {
      runLeft = (uint8_t)random(2, 4);
      runGap = gap; runW = w;
    }
  }

  if (nseg >= MAX_SEG) return;
  segs[nseg++] = { prevEnd + gap, w, top };
  const Seg &s = segs[nseg - 1];

  if (gap > 0.0f) {
    // Coins over the gap, following the jump that clears it. The arc starts a
    // little BEFORE the lip: jumping slightly early is the safe way to clear
    // a gap, so the coins should reward that rather than punish it.
    coinArc(prevEnd - 26.0f, (float)p.top, 4);
    nextHazardX = s.x + sep;            // clear ground after the landing edge
  }

  placeHazard(s);

  // A reward says "jump here". It must never say that where a bird is
  // waiting -- a low bird sits from top-54 to top-28 and a coin row sits at
  // top-34, so without this check the two can overlap exactly and the coins
  // become bait.
  const float rx = s.x + frnd(40.0f, fmaxf(45.0f, s.w - 60.0f));
  bool clear = true;
  for (uint8_t i = 0; i < nbird; i++)
    if (birds[i].x + BIRD_W > rx - 52.0f && birds[i].x < rx + 106.0f) clear = false;
  if (!clear) return;

  // Rare treasure, in rising order of rarity. A heart is the only way to get
  // a life back, so it stays genuinely uncommon.
  if (level >= LV_BIRDS_ALL && random(1000) < 6) {
    addPickup(rx, (float)s.top - 42.0f, PK_HEART);
  } else if (level >= LV_CRATES && random(100) < 9) {
    addPickup(rx, (float)s.top - 40.0f, PK_GEM);
  } else if (random(100) < 40) {
    for (uint8_t i = 0; i < 3; i++)
      addCoin(rx + i * 18.0f, (float)s.top - 34.0f);
  }
}

// Roughly one per level, at an unpredictable point inside it, so it is a
// thing you happen to catch rather than a thing that happens on schedule.
static void scheduleEgg() {
  eggNextDist = distance + frnd(150.0f, LEVEL_DIST * 0.80f);
}

static void updateEgg(float dt) {
  if (!egg.active) {
    if (distance < eggNextDist) return;
    egg.kind  = (uint8_t)random(EGG_KINDS);
    egg.phase = 0.0f;
    const int8_t d = (random(100) < 50) ? 1 : -1;
    // Slow enough to notice and follow across the sky -- except the shooting
    // star, which is meant to be a "did you see that?".
    const float sp = (egg.kind == EGG_PLANE) ? 30.0f
                   : (egg.kind == EGG_SAT)   ? 18.0f
                   : (egg.kind == EGG_UFO)   ? 38.0f : 150.0f;
    const int16_t hi = (egg.kind == EGG_SAT) ? 28 : (egg.kind == EGG_UFO) ? 52 : 44;
    egg.y  = frnd(10.0f, (float)hi);
    egg.vx = sp * d;
    egg.x  = (d > 0) ? -24.0f : (float)(PLAY_W + 24);
    egg.active = true;
    eggNextDist = 1e9f;      // exactly one per level; the next level re-arms it
    Serial.printf("[game] sky: %s\n",
                  egg.kind == EGG_PLANE ? "airplane" :
                  egg.kind == EGG_SAT   ? "satellite" :
                  egg.kind == EGG_UFO   ? "UFO" : "shooting star");
    return;
  }
  egg.x     += egg.vx * dt;
  egg.phase += 2.2f * dt;
  if (egg.x < -40.0f || egg.x > PLAY_W + 40.0f) egg.active = false;
}

static void cullAndTop() {
  const float left = camX - 60.0f;
  uint8_t k = 0;
  for (uint8_t i = 0; i < nseg; i++)
    if (segs[i].x + segs[i].w > left) segs[k++] = segs[i];
  nseg = k;

  k = 0;
  for (uint8_t i = 0; i < ncoin; i++)
    if (coins[i].alive && coins[i].x > left) coins[k++] = coins[i];
  ncoin = k;

  k = 0;
  for (uint8_t i = 0; i < ncrate; i++)
    if (crates[i].x + CRATE_S > left) crates[k++] = crates[i];
  ncrate = k;

  k = 0;
  for (uint8_t i = 0; i < nbird; i++)
    if (birds[i].alive && birds[i].x + BIRD_W > left) birds[k++] = birds[i];
  nbird = k;

  while (nseg > 0 && nseg < MAX_SEG &&
         segs[nseg - 1].x + segs[nseg - 1].w < camX + PLAY_W + 220.0f)
    addSegment();
}

// Highest platform surface under [x0, x1], or NO_GROUND if it is all gap.
static const int16_t NO_GROUND = 9999;
static int16_t surfaceAt(float x0, float x1) {
  int16_t best = NO_GROUND;
  for (uint8_t i = 0; i < nseg; i++)
    if (x1 > segs[i].x && x0 < segs[i].x + segs[i].w && segs[i].top < best)
      best = segs[i].top;
  return best;
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------
static void sfxJump()   { synthBeep(76, 45, WAVE_SQUARE, 70); synthBeep(83, 35, WAVE_SQUARE, 60); }
static void sfxDouble() { const uint8_t a[] = {84, 89, 93}; synthArp(a, 3, 32, WAVE_SQUARE, 70); }
static void sfxCoin()   { synthBeep(93, 30, WAVE_SQUARE, 65); synthBeep(100, 55, WAVE_SQUARE, 65); }
static void sfxStomp()  { synthBeep(59, 40, WAVE_SQUARE, 90); synthBeep(47, 60, WAVE_TRIANGLE, 90); }
static void sfxHurt()   { const uint8_t a[] = {64, 59, 52, 45}; synthArp(a, 4, 80, WAVE_TRIANGLE, 95); }
static void sfxLevel()  { const uint8_t a[] = {72, 76, 79, 84}; synthArp(a, 4, 70, WAVE_SQUARE, 80); }
static void sfxOver()   { const uint8_t a[] = {69, 65, 62, 57, 50}; synthArp(a, 5, 130, WAVE_TRIANGLE, 100); }

// ---------------------------------------------------------------------------
// Particles
// ---------------------------------------------------------------------------
static void puff(float x, float y, uint16_t col, uint8_t n) {
  for (uint8_t i = 0, made = 0; i < MAX_PART && made < n; i++)
    if (parts[i].life == 0) {
      parts[i] = { x, y, frnd(-70, 70), frnd(-110, -20),
                   (uint8_t)random(7, 13), 3, col };
      made++;
    }
}

// A proper explosion: particles thrown in every direction at real speed, in
// mixed sizes and colours, rather than the polite little upward puff.
static void burst(float x, float y, uint8_t n, uint16_t a, uint16_t b) {
  for (uint8_t i = 0, made = 0; i < MAX_PART && made < n; i++)
    if (parts[i].life == 0) {
      const float ang = frnd(0.0f, 6.2832f), sp = frnd(60.0f, 260.0f);
      parts[i] = { x, y, cosf(ang) * sp, sinf(ang) * sp - 40.0f,
                   (uint8_t)random(10, 22), (uint8_t)random(2, 6),
                   (made & 1) ? a : b };
      made++;
    }
}

// Screen shake and a flash frame, both decaying in seconds.
static float shakeT = 0.0f, flashT = 0.0f;
static void kick(float shake, float flash) {
  if (shake > shakeT) shakeT = shake;
  if (flash > flashT) flashT = flash;
}

// Hitstop: the whole world holds still for a few dozen milliseconds on a big
// hit. It is the cheapest trick in game feel and by far the most effective --
// the pause is what makes an impact land instead of just happening.
static float freezeT = 0.0f;

// An expanding shockwave ring, and a score number that floats off the hit.
static float ringT = 0.0f, ringX = 0.0f, ringY = 0.0f;
static float popT  = 0.0f, popX  = 0.0f, popY  = 0.0f;
static char  popText[12];

static void celebrate(float x, float y, float power, int points) {
  ringT = 0.30f; ringX = x; ringY = y;
  popT  = 0.85f; popX  = x; popY  = y - 24.0f;   // clear of the debris
  snprintf(popText, sizeof(popText), "+%d", points);
  freezeT = 0.05f + power * 0.03f;
}

// ---------------------------------------------------------------------------
// Reset / start
// ---------------------------------------------------------------------------
// How long the menu sits still before the bot starts playing behind it.
static const uint32_t ATTRACT_AFTER_MS = 5000;
static bool attract = false;          // a demo game is running under the menu

static void newGame() {
  nseg = ncoin = ncrate = nbird = 0;
  for (uint8_t i = 0; i < MAX_PART; i++) parts[i].life = 0;

  camX = 0; speed = SPEED_0; level = 1; score = 0; scoreAcc = 0; coinsGot = 0;
  lives = START_LIVES; distance = 0; birdCombo = 0;
  runLeft = 0; nextHazardX = -1e9f;
  shakeT = flashT = freezeT = ringT = popT = 0.0f;
  attract = botDrive = false;   // a real game; startAttract re-arms them
  egg.active = false; scheduleEgg();
  invuln = 0; coyote = 0; buffered = 0; usedDouble = false; holding = false;

  // Three flat, gapless segments to start on -- nothing to fail at yet, and a
  // low row of coins so the very first thing that happens is a small win.
  // Two flat segments, not three: the old intro was 1060 px and level 1 is
  // only 900, so the tutorial WAS the whole first level -- measured as 10.3 s
  // with zero jumps and zero deaths.
  segs[nseg++] = { -80.0f, 400.0f, 138 };
  segs[nseg++] = { 320.0f, 260.0f, 138 };
  py = 138 - PLR_H; vy = 0; grounded = true;
  // At running height, not a hop: the very first coins of a run should be a
  // free win for a four-year-old who has not worked out the button yet.
  for (uint8_t i = 0; i < 4; i++) addCoin(250.0f + i * 20.0f, 138.0f - 28.0f);
  cullAndTop();

#ifdef GAME_HOST
  { void gameStatReached(uint8_t); gameStatReached(1); }
#endif
  g_lastHud = 0xFFFFFFFF;
  banner[0] = 0;
  state = ST_PLAY; stateSince = millis();
  synthHoldAmp(true);
  Serial.printf("[game] start: jump height %.0f px, airtime %.2f s, "
                "reach at level 1 = %.0f px\n", JUMP_H, AIRTIME, SPEED_0 * AIRTIME);
}

static void hurt(const char *why) {
  if (invuln > 0.0f) return;
  STAT_DEATH(why[0] == 'w' ? 0 : (why[0] == 'c' ? 1 : 2));
  lives = (lives > 0) ? lives - 1 : 0;
  invuln = 1.6f;
  birdCombo = 0;
  vy = -180.0f;
  level = (level > 1) ? level - 1 : 1;             // give a little slack back
  speed = fminf(SPEED_0 * powf(SPEED_STEP, level - 1), SPEED_MAX);
  puff(PLAYER_X + PLR_W / 2, py + PLR_H / 2, C_MAG, 10);
  snprintf(banner, sizeof(banner), "OUCH!");
  bannerUntil = millis() + 900;
  sfxHurt();
  g_lastHud = 0xFFFFFFFF;
  Serial.printf("[game] hit (%s), %u lives left\n", why, lives);

  if (lives == 0) {
    state = ST_OVER; stateSince = millis();
    if (score > best) { best = score; prefs.begin("mluvitko", false);
                        prefs.putUInt("best", best); prefs.end(); }
    sfxOver();
    Serial.printf("[game] game over: score %lu, best %lu, %u m\n",
                  (unsigned long)score, (unsigned long)best,
                  (unsigned)(distance / 32));
  }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static bool     btnStable = true, btnRaw = true;   // pull-up: true = released
static uint32_t btnChanged = 0;

// Returns true on the falling edge (a fresh press).
static bool readButton() {
  const bool r = digitalRead(PIN_BUTTON);
  if (r != btnRaw) { btnRaw = r; btnChanged = millis(); }
  if (millis() - btnChanged > 15 && r != btnStable) {
    btnStable = r;
    holding = !btnStable;
    return !btnStable;
  }
  return false;
}

// ---------------------------------------------------------------------------
// The bot.
//
// It drives the SAME virtual button a child does -- it calls tryJump() and
// sets `holding`, so it is subject to every rule the player is: debounce-free
// but coyote time, jump buffering and hold-for-height all apply. That is the
// point: a bot that cheated its way past the physics would measure nothing.
//
// It is used for two things: the arcade attract mode on the board, and
// measuring how hard the game actually is on the desktop harness.
//
// Skill is one knob -- timing error -- plus a "sloppy" rate of needless jumps.
// Those two are exactly the ways a small child fails: mistimed take-offs, and
// pressing the button when nothing asked them to (which the low birds punish).
// ---------------------------------------------------------------------------
// The next thing ahead that actually requires a jump, and how far away it is.
// Birds are deliberately absent: every bird is placed above a standing
// runner's head, so NOT jumping is always safe, and the separation invariant
// guarantees no bird ever sits inside a jump the terrain forces. The bot
// therefore never needs to react to one -- it only has to not be silly.
static uint8_t botScan(float *distOut) {
  const float wx = camX + PLAYER_X;
  const float horizon = speed * 1.3f;
  uint8_t kind = 0;
  float   best = 1e9f;

  for (uint8_t i = 1; i < nseg; i++) {
    const float g0 = segs[i - 1].x + segs[i - 1].w;
    if (segs[i].x > g0) {                       // a real gap
      const float d = g0 - (wx + PLR_W * 0.5f);
      if (d > -6.0f && d < horizon && d < best) { best = d; kind = 1; }
    }
  }
  for (uint8_t i = 0; i < ncrate; i++) {
    const float d = crates[i].x - (wx + PLR_W);
    if (d > -6.0f && d < horizon && d < best) { best = d; kind = 2; }
  }
  // Step-ups need a hop too. Leaving these out was why the bot looked no
  // better than a masher: it was walking into every ledge in the game.
  for (uint8_t i = 1; i < nseg; i++) {
    if (segs[i].top < segs[i - 1].top - 5 && segs[i].x <= segs[i - 1].x + segs[i - 1].w + 1.0f) {
      const float d = segs[i].x - (wx + PLR_W);
      if (d > -6.0f && d < horizon && d < best) { best = d; kind = 2; }
    }
  }
  *distOut = best;
  return kind;
}

static void tryJump();

// How far the runner rises above its take-off point, t seconds into a jump
// held for `hold` seconds. This is the player's own physics solved forward,
// which is what lets the bot aim a jump at something rather than guess.
static float jumpRise(float t, float hold) {
  const float v0 = -JUMP_V0;                       // 335 px/s, upward
  const float th = fminf(hold, v0 / G_UP_HELD);    // held phase, capped at apex
  if (t <= th) return v0 * t - 0.5f * G_UP_HELD * t * t;

  float y  = v0 * th - 0.5f * G_UP_HELD * th * th;
  float v  = v0 - G_UP_HELD * th;                  // still climbing if positive
  float tr = t - th;
  if (v > 0.0f) {
    const float tp = v / G_UP_FREE;
    if (tr <= tp) return y + v * tr - 0.5f * G_UP_FREE * tr * tr;
    y  += 0.5f * v * tp;
    tr -= tp;
  }
  return y - 0.5f * G_DOWN * tr * tr;
}

// Jumping is only ever a mistake when a bird is overhead, so that is the only
// thing a discretionary jump has to check.
static bool botSafeToJump() {
  const float wx = camX + PLAYER_X;
  const float arc = speed * AIRTIME;
  for (uint8_t i = 0; i < nbird; i++)
    if (birds[i].x + BIRD_W > wx - 10.0f && birds[i].x < wx + arc + 10.0f)
      return false;
  return true;
}

// The nearest coin a single jump from here would actually pass through, and
// how long to hold for it. Solving the arc instead of hopefully hopping is
// what turns "collects some coins by accident" into "goes and gets them".
static bool botCoinJump(float *holdOut) {
  const float wx   = camX + PLAYER_X;
  const float feet = py + PLR_H;
  static const float HOLDS[4] = { 0.08f, 0.16f, 0.26f, 0.40f };

  float bestDx = 1e9f;
  bool  found  = false;

  for (uint8_t i = 0; i < ncoin; i++) {
    if (!coins[i].alive) continue;
    // Coins at running height get collected without doing anything.
    if (coins[i].y + COIN_S > feet - PLR_H) continue;

    const float dx = (coins[i].x + COIN_S * 0.5f) - (wx + PLR_W * 0.5f);
    if (dx < 4.0f || dx >= bestDx) continue;
    const float t = dx / speed;
    if (t > AIRTIME * 0.95f) continue;             // not reachable in one jump

    for (uint8_t h = 0; h < 4; h++) {
      const float top = feet - PLR_H - jumpRise(t, HOLDS[h]);
      if (top < coins[i].y + COIN_S && top + PLR_H > coins[i].y) {
        bestDx = dx; *holdOut = HOLDS[h]; found = true;
        break;
      }
    }
  }
  return found;
}

static void botThink(float dt) {
  if (botHoldT > 0.0f) { botHoldT -= dt; botHolding = true; }
  else botHolding = false;

  // Press the button for no reason now and then. This is not noise for its
  // own sake -- it is the behaviour the low birds exist to punish, so it is
  // what makes the difficulty numbers mean something for a small child.
  botSloppyT -= dt;
  if (botSkill.sloppyPct && botSloppyT <= 0.0f) {
    botSloppyT = 0.35f;
    if (random(100) < (int)(botSkill.sloppyPct * 0.35f)) {
      tryJump(); botHoldT = 0.10f; botHolding = true;
      return;
    }
  }

  if (!grounded && coyote <= 0.0f) return;      // nothing to start from

  float d;
  const uint8_t kind = botScan(&d);
  if (kind == 0) return;

  // Take off slightly before the obstacle. The lead is in seconds so it
  // scales with speed, and the jitter is the skill knob: a late take-off
  // drowns you, an early one lands you short.
  const float lead = speed * (0.10f + frnd(-botSkill.jitter, botSkill.jitter));
  if (kind != 0 && d <= lead) {
    tryJump();
    botHoldT   = (kind == 1) ? 0.26f : 0.15f;   // gap = full jump, crate = hop
    botHolding = true;
    return;
  }

  // Nothing urgent. Survival is not the only thing worth playing for, so go
  // and get coins -- but only with the whole arc clear of the next hazard,
  // and never with a bird overhead. Skill gates this too: a child who cannot
  // time a jump well should not be reliably vacuuming up coins either.
  if (kind != 0 && d < speed * AIRTIME * 1.15f) return;
  if (botSkill.jitter > 0.02f && random(100) < (int)(botSkill.jitter * 420.0f)) return;

  float hold;
  if (botSafeToJump() && botCoinJump(&hold)) {
    tryJump();
    botHoldT   = hold;
    botHolding = true;
  }
}

// Start a demo: same game, same rules, bot on the sticks.
static void startAttract() {
  newGame();
  attract  = true;
  botDrive = true;
  botSkill = BOT_PROFILES[1];       // plays like a competent 8-year-old
  Serial.println(F("[game] attract mode: bot playing"));
}

static void tryJump() {
  STAT(statJumps);
  if (grounded || coyote > 0.0f) {
    vy = JUMP_V0; grounded = false; coyote = 0; usedDouble = false;
    birdCombo = 0;
    puff(PLAYER_X + PLR_W / 2, py + PLR_H, C_CYAN, 4);
    sfxJump();
  } else if (!usedDouble) {
    vy = DBL_V0; usedDouble = true;
    puff(PLAYER_X + PLR_W / 2, py + PLR_H, C_YELLOW, 7);
    sfxDouble();
  } else {
    buffered = 0.14f;    // pressed too early -- remember it until we land
  }
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------
static void step(float dt) {
  // --- horizontal: the world moves, the runner does not ---
  camX     += speed * dt;
  distance += speed * dt;
  STAT(statFrames);

  const uint8_t want = (uint8_t)(distance / LEVEL_DIST) + 1;
  if (want > level) {
    level = want;
    speed = fminf(SPEED_0 * powf(SPEED_STEP, level - 1), SPEED_MAX);
#ifdef GAME_HOST
    { void gameStatReached(uint8_t); gameStatReached(level); }
#endif
    scheduleEgg();                     // one sighting per level
    snprintf(banner, sizeof(banner), "LEVEL %u", level);
    bannerUntil = millis() + 1200;
    sfxLevel();
    Serial.printf("[game] level %u, speed %.0f px/s, max gap %.0f px\n",
                  level, speed, maxGapFor(0));
  }
  cullAndTop();
  updateEgg(dt);

  // --- vertical ---
  const float g = (vy < 0.0f) ? (holding ? G_UP_HELD : G_UP_FREE) : G_DOWN;
  vy = fminf(vy + g * dt, VY_MAX);
  const float prevBottom = py + PLR_H;
  py += vy * dt;

  // Support: sample the player's left, middle and right so landing on the
  // very lip of a platform still counts. Being generous here is the single
  // biggest difference between "fun" and "unfair" for a small child.
  const float wx = camX + PLAYER_X;
  int16_t sup = surfaceAt(wx + 2, wx + PLR_W - 2);
  for (uint8_t i = 0; i < ncrate; i++) {
    const Crate &c = crates[i];
    if (wx + PLR_W - 2 > c.x && wx + 2 < c.x + CRATE_S && c.top < sup) sup = c.top;
  }

  // Ground under the CENTRE only. The wide sample above is deliberately
  // forgiving about landing on a lip, but it also straddles a small gap, so
  // it must never be what decides you are standing on something. The centre
  // sample is what tells a real platform from a hole.
  int16_t supMid = surfaceAt(wx + PLR_W * 0.5f - 2.0f, wx + PLR_W * 0.5f + 2.0f);
  for (uint8_t i = 0; i < ncrate; i++) {
    const Crate &c = crates[i];
    if (wx + PLR_W * 0.5f + 2.0f > c.x && wx + PLR_W * 0.5f - 2.0f < c.x + CRATE_S
        && c.top < supMid) supMid = c.top;
  }

  const bool wasGrounded = grounded;
  grounded = false;

  // Two different ways to end up standing, needing two different tolerances.
  //
  // 1. Landing from above (wide sample, TIGHT tolerance): you must have been
  //    above the surface last frame. Without that test the wide foot sampling
  //    rescues you from a narrow gap, because your shoulders still overlap
  //    the far bank -- the shallow-water bug, where small ponds did not kill.
  //
  // 2. Climbing a step that scrolled INTO you (centre sample, generous
  //    tolerance): the world moves and the runner cannot back away, so a step
  //    up you failed to clear leaves you embedded in its wall, already well
  //    below its surface. Without this branch you then sink THROUGH solid
  //    rock into the water -- which turned out to be ~97% of all drownings,
  //    none of them anywhere near a gap. It cannot rescue anyone from a hole,
  //    because it requires your centre to be over solid ground.
  const float SNAP_TOL  = 9.0f;
  const float CLIMB_TOL = maxRise() + 10.0f;
  const bool  landing = (sup != NO_GROUND && py + PLR_H >= (float)sup &&
                         prevBottom <= (float)sup + SNAP_TOL);
  const bool  climbing = (supMid != NO_GROUND && py + PLR_H >= (float)supMid &&
                          py + PLR_H <= (float)supMid + CLIMB_TOL);
  if (climbing && (!landing || supMid < sup)) sup = supMid;

  if (vy >= 0.0f && (landing || climbing) && sup != NO_GROUND) {
    py = (float)sup - PLR_H;
    if (vy > 260.0f) puff(PLAYER_X + PLR_W / 2, py + PLR_H, C_CYAN, 3);
    vy = 0; grounded = true; usedDouble = false; birdCombo = 0;
  }

  coyote = grounded ? 0.09f : fmaxf(0.0f, coyote - dt);
  if (wasGrounded && !grounded && vy >= 0.0f) coyote = 0.09f;
  if (buffered > 0.0f) {
    buffered -= dt;
    if (grounded) { buffered = 0; tryJump(); }
  }
  if (invuln > 0.0f) invuln -= dt;

  // --- hit the water ---
  // Every platform surface sits above WATER_Y, so feet at the waterline can
  // only mean there is nothing underneath. No invulnerability frames here:
  // water always kills, or the rule stops being a rule.
  if (py + PLR_H >= (float)WATER_Y) {
    for (uint8_t i = 0; i < 3; i++)
      puff(PLAYER_X + PLR_W / 2, (float)WATER_Y, C_CYAN, 4);
    puff(PLAYER_X + PLR_W / 2, (float)WATER_Y, C_WHITE, 4);
    synthBeep(52, 70, WAVE_TRIANGLE, 90);
    synthBeep(40, 120, WAVE_TRIANGLE, 90);
    invuln = 0;
    hurt("water");
    if (state == ST_OVER) return;
    // Put them back on the next real ground ahead, mid-air, still moving.
    bool placed = false;
    for (uint8_t i = 0; i < nseg; i++)
      if (segs[i].x > camX + PLAYER_X) {
        camX = segs[i].x - PLAYER_X + 8.0f;
        py   = segs[i].top - PLR_H - 40.0f;
        placed = true;
        break;
      }
    // Nothing generated ahead yet: park them safely rather than leaving the
    // feet under the waterline, which would re-drown them every frame and
    // burn all three lives in well under a second.
    if (!placed) py = (float)(TOP_MIN - PLR_H - 20);
    vy = 0;
  }

  // --- crates: side hit ---
  for (uint8_t i = 0; i < ncrate; i++) {
    const Crate &c = crates[i];
    if (wx + PLR_W - 3 > c.x && wx + 3 < c.x + CRATE_S &&
        py + PLR_H > c.top + 7.0f && py < c.top + CRATE_S)
      hurt("crate");
  }

  // --- birds ---
  // Birds are placed by the world generator (see placeHazard) and are STATIC
  // in world space -- they hover and bob, they do not fly towards you. That
  // is deliberate: a bird drifting at its own speed re-times itself against
  // terrain that was laid out much earlier, and will eventually park itself
  // over a crate. A crate forces a jump, a low bird punishes one, so that
  // combination is unclearable. Static placement is what makes the
  // separation guarantee hold for the whole life of the bird.
  for (uint8_t i = 0; i < nbird; i++) {
    Bird &b = birds[i];
    b.phase += 6.0f * dt;
    const float by = b.y + sinf(b.phase) * 5.0f;

    // Ran underneath it without jumping. That is exactly the skill the low
    // birds exist to teach, so it pays -- otherwise the only feedback for
    // playing correctly is the absence of punishment, which teaches nothing.
    if (b.alive && !b.scored && grounded && b.x + BIRD_W < wx + PLR_W * 0.5f &&
        by + BIRD_H > py - 26.0f) {
      b.scored = true;
      score += 30;
      snprintf(banner, sizeof(banner), "COOL!");
      bannerUntil = millis() + 700;
      puff(PLAYER_X + PLR_W / 2, py - 6.0f, C_CYAN, 4);
      synthBeep(88, 40, WAVE_SQUARE, 55);
    }
    if (wx + PLR_W - 3 > b.x && wx + 3 < b.x + BIRD_W &&
        py + PLR_H > by && py < by + BIRD_H) {
      if (vy > 40.0f && py + PLR_H < by + BIRD_H * 0.8f) {
        b.alive = false;
        vy = STOMP_V0; usedDouble = false;
        birdCombo++;
        score += 50 * birdCombo;

        // Feathers everywhere, a flash, and a thump you can feel. Particles
        // live in screen space, hence the -camX.
        const float bx = b.x - camX + BIRD_W / 2;
        burst(bx, by, 18 + birdCombo * 6, C_WHITE, C_MAG);
        burst(bx, by, 8, C_CYAN, C_YELLOW);
        kick(0.16f + birdCombo * 0.05f, 0.07f);
        sfxStomp();

        celebrate(bx, by, (float)birdCombo, 50 * birdCombo);

        static const char *COMBO[] = { "SPLAT!", "DOUBLE!", "TRIPLE!", "INSANE!!" };
        snprintf(banner, sizeof(banner), "%s",
                 COMBO[birdCombo > 4 ? 3 : birdCombo - 1]);
        bannerUntil = millis() + 900 + birdCombo * 120;
      } else hurt("bird");
    }
  }

  // --- coins ---
  for (uint8_t i = 0; i < ncoin; i++) {
    Coin &c = coins[i];
    if (!c.alive) continue;
    if (wx + PLR_W - 2 > c.x && wx + 2 < c.x + COIN_S &&
        py + PLR_H > c.y && py < c.y + COIN_S) {
      // Worth enough that collecting is a real strategy, not a rounding error
      // next to distance points.
      c.alive = false;
      const float px = c.x - camX + COIN_S / 2, pyv = c.y + COIN_S / 2;
      if (c.kind == PK_GEM) {
        score += GEM_POINTS;
        burst(px, pyv, 16, C_CYAN, C_WHITE);
        kick(0.10f, 0.05f);
        snprintf(banner, sizeof(banner), "GEM!");
        bannerUntil = millis() + 900;
        const uint8_t a[] = { 84, 88, 91, 96 };
        synthArp(a, 4, 45, WAVE_SQUARE, 75);
      } else if (c.kind == PK_HEART) {
        if (lives < MAX_LIVES) lives++;
        g_lastHud = 0xFFFFFFFF;
        burst(px, pyv, 20, C_MAG, C_WHITE);
        kick(0.14f, 0.08f);
        snprintf(banner, sizeof(banner), "1 UP!");
        bannerUntil = millis() + 1300;
        const uint8_t a[] = { 72, 79, 84, 88, 91 };
        synthArp(a, 5, 60, WAVE_SQUARE, 85);
      } else {
        coinsGot++; score += COIN_POINTS; STAT(statCoins);
        puff(px, pyv, C_YELLOW, 4);
        sfxCoin();
      }
    }
  }

  shakeT = fmaxf(0.0f, shakeT - dt);
  flashT = fmaxf(0.0f, flashT - dt);
  ringT  = fmaxf(0.0f, ringT  - dt);
  if (popT > 0.0f) { popT -= dt; popY -= 34.0f * dt; }

  // --- particles ---
  for (uint8_t i = 0; i < MAX_PART; i++) {
    if (!parts[i].life) continue;
    parts[i].x  += (parts[i].vx - speed) * dt;
    parts[i].y  += parts[i].vy * dt;
    parts[i].vy += 500.0f * dt;
    parts[i].life--;
  }

  // Distance points, accumulated as a float: at level 1 this is well under
  // one point per frame, so truncating each frame would score exactly zero.
  scoreAcc += speed * dt * 0.12f;
  if (scoreAcc >= 1.0f) { score += (uint32_t)scoreAcc; scoreAcc -= floorf(scoreAcc); }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
static void buildBackgroundTables() {
  for (int16_t y = 0; y < PLAY_H; y++) {
    if (y < HORIZON) {
      const float t = (float)y / HORIZON;
      // indigo -> violet -> magenta -> hot orange at the horizon
      if (t < 0.45f) skyLUT[y] = lerpRGB(20, 0, 48, 96, 12, 116, t / 0.45f);
      else if (t < 0.80f) skyLUT[y] = lerpRGB(96, 12, 116, 226, 40, 110, (t - 0.45f) / 0.35f);
      else skyLUT[y] = lerpRGB(226, 40, 110, 255, 138, 30, (t - 0.80f) / 0.20f);
    } else {
      const float t = (float)(y - HORIZON) / (PLAY_H - HORIZON);
      skyLUT[y] = lerpRGB(30, 4, 44, 4, 0, 12, t);
    }
  }
  for (int16_t y = 0; y < PLAY_H; y++) {
    const float t = constrain((float)(y - TOP_MIN) / (float)(PLAY_H - TOP_MIN),
                              0.0f, 1.0f);
    rockLUT[y] = lerpRGB(56, 12, 84, 8, 0, 20, t);   // lit face -> shadow
  }
  for (uint16_t i = 0; i < MTN_N; i++) {
    const float a = sinf(i * 0.043f) * 12.0f + sinf(i * 0.011f) * 16.0f
                  + sinf(i * 0.131f) * 5.0f;
    mtn[i] = (uint8_t)constrain(20.0f + a, 2.0f, 44.0f);
  }
}

// Each drawn around a centre point, with d = +1 travelling right, -1 left.
static void eggPlane(int16_t x, int16_t y, int8_t d) {
  for (uint8_t i = 1; i <= 5; i++) {              // contrail, fading behind
    const int16_t tx = x - d * (8 + i * 5);
    const uint16_t c = (i < 3) ? 0x8410 : 0x4208;
    cv->drawPixel(tx, y + 3, c);
    cv->drawPixel(tx + 1, y + 3, c);
  }
  cv->fillRect(x - 6, y + 2, 12, 3, C_WHITE);     // fuselage
  cv->fillRect(x + d * 5, y + 3, 3, 2, 0xC618);   // nose
  cv->fillRect(x - 1, y, 3, 8, 0xC618);           // wings
  cv->fillRect(x - d * 7, y, 2, 3, 0xC618);       // tail fin
  if ((millis() / 350) & 1) cv->drawPixel(x + d * 6, y + 3, C_MAG);   // beacon
}

static void eggSat(int16_t x, int16_t y) {
  cv->fillRect(x - 3, y, 6, 6, 0xC618);           // body
  cv->fillRect(x - 11, y + 1, 7, 4, C_CYAN);      // solar panels
  cv->fillRect(x + 5, y + 1, 7, 4, C_CYAN);
  cv->drawFastHLine(x - 11, y + 2, 7, 0x035F);
  cv->drawFastHLine(x + 5, y + 2, 7, 0x035F);
  if ((millis() / 500) & 1) cv->drawPixel(x, y - 2, C_WHITE);
}

static void eggUfo(int16_t x, int16_t y) {
  cv->fillRect(x - 3, y - 3, 7, 3, C_CYAN);       // dome
  cv->fillRect(x - 9, y, 19, 3, 0xC618);          // saucer
  cv->fillRect(x - 6, y + 3, 13, 2, 0x8410);
  const uint8_t f = (millis() / 140) % 3;
  for (uint8_t i = 0; i < 3; i++)
    cv->drawPixel(x - 5 + i * 5, y + 5, (i == f) ? C_YELLOW : C_MAG);
}

static void eggStar(int16_t x, int16_t y, int8_t d) {
  for (uint8_t i = 0; i < 15; i++) {
    const uint16_t c = (i < 3) ? C_WHITE : (i < 8 ? 0xAD7F : 0x4A3F);
    cv->drawPixel(x - d * i, y + i / 3, c);
  }
}

static void drawEgg() {
  if (!egg.active) return;
  const int16_t x = (int16_t)egg.x;
  const int8_t  d = (egg.vx >= 0.0f) ? 1 : -1;
  const int16_t y = (int16_t)(egg.y + ((egg.kind == EGG_UFO)
                                       ? sinf(egg.phase) * 5.0f : 0.0f));
  switch (egg.kind) {
    case EGG_PLANE: eggPlane(x, y, d); break;
    case EGG_SAT:   eggSat(x, y);      break;
    case EGG_UFO:   eggUfo(x, y);      break;
    default:        eggStar(x, y, d);  break;
  }
}

static void drawSun() {
  for (int16_t dy = -SUN_R; dy <= SUN_R; dy++) {
    const int16_t y = SUN_Y + dy;
    if (y < 0 || y >= HORIZON) continue;

    // The classic synthwave sun: solid on top, then horizontal slits that get
    // thicker the further down you go, so it dissolves into the horizon.
    if (dy > -6) {
      const int16_t period = 8;
      const int16_t cut    = 1 + (dy + 6) / 10;      // 1..4 rows removed
      if ((dy + 6) % period < cut) continue;
    }
    const int16_t half = (int16_t)sqrtf((float)(SUN_R * SUN_R - dy * dy));
    const float   t    = (float)(dy + SUN_R) / (2 * SUN_R);
    cv->drawFastHLine(SUN_X - half, y, half * 2,
                      lerpRGB(255, 236, 90, 255, 60, 110, t));
  }
}

static void drawBackground() {
  for (int16_t y = 0; y < PLAY_H; y++)
    cv->drawFastHLine(0, y, PLAY_W, skyLUT[y]);

  // Stars in the deep part of the sky, drifting slower than anything else.
  // Cheap depth, and it stops the top third being an empty purple slab.
  const int32_t so = (int32_t)(camX * 0.08f);
  for (uint8_t i = 0; i < 26; i++) {
    const int16_t sx = (int16_t)(((i * 71 + 13) - so) % 320 + (so < 0 ? 320 : 0));
    const int16_t sy = 4 + (i * 37) % 62;
    if (sx >= 0 && sx < PLAY_W)
      cv->drawPixel(sx, sy, (i & 3) ? 0x6B5D : C_WHITE);
  }

  drawSun();
  drawEgg();          // in front of the sun, behind the mountains it sets into

  // Mountains, drifting slowly (parallax) so depth reads even at 20 fps.
  const int32_t mo = (int32_t)(camX * 0.30f);
  for (int16_t x = 0; x < PLAY_W; x++) {
    const uint8_t h = mtn[(uint16_t)((mo + x) % MTN_N)];
    cv->drawFastVLine(x, HORIZON - h, h, C_MTN);
    cv->drawPixel(x, HORIZON - h, C_MAG);
  }

  // Neon floor grid. Deliberately starts BELOW the lowest platform surface,
  // so it is only ever seen through a pit -- never as a stripe of fake floor
  // hanging in the air where the player can't actually stand.
  const int16_t GRID_TOP = TOP_MAX + 2;
  for (uint8_t k = 0; k < 8; k++) {
    const int16_t y = GRID_TOP + 2 + (int16_t)(k * k * 0.9f);
    if (y >= PLAY_H) break;
    cv->drawFastHLine(0, y, PLAY_W, C_GRID);
  }
  const float vo = fmodf(camX * 0.6f, 40.0f);
  for (int8_t i = -4; i <= 12; i++) {
    const float xb = i * 40.0f - vo;
    const float xh = 160.0f + (xb - 160.0f) * 0.25f;   // converge on the centre
    for (int16_t y = GRID_TOP; y < WATER_Y; y++) {
      const float t = (float)(y - GRID_TOP) / (PLAY_H - GRID_TOP);
      cv->drawPixel((int16_t)(xh + (xb - xh) * t), y, C_GRID);
    }
  }

  // Water. Drawn full width here and then covered by the platforms, so it
  // shows through exactly where there is a gap -- and nowhere else.
  for (int16_t y = WATER_Y; y < PLAY_H; y++) {
    const float t = (float)(y - WATER_Y) / (float)(PLAY_H - WATER_Y);
    cv->drawFastHLine(0, y, PLAY_W, lerpRGB(0, 60, 130, 0, 10, 48, t));
  }
  cv->drawFastHLine(0, WATER_Y, PLAY_W, C_CYAN);          // bright surface line
  const float ph = millis() / 220.0f;
  for (int16_t x = 0; x < PLAY_W; x += 2) {               // moving glints
    const int16_t wy = WATER_Y + 4 + (int16_t)(2.5f * (1.0f + sinf(x * 0.09f + ph)));
    cv->drawPixel(x, wy, 0x4E5F);
    const int16_t wy2 = WATER_Y + 13 + (int16_t)(3.0f * (1.0f + sinf(x * 0.06f - ph * 1.3f)));
    if (wy2 < PLAY_H) cv->drawPixel(x, wy2, 0x2D3F);
  }
}

static void drawWorld() {
  // Shake displaces the WORLD only -- the sky and sun stay put, which reads as
  // the ground being hit rather than the camera being broken.
  const float camX = ::camX + (shakeT > 0.0f
      ? sinf(millis() * 0.09f) * (shakeT * 26.0f) : 0.0f);
  // Platforms
  for (uint8_t i = 0; i < nseg; i++) {
    const int16_t x = (int16_t)(segs[i].x - camX);
    const int16_t w = (int16_t)segs[i].w;
    const int16_t t = segs[i].top;
    if (x > PLAY_W || x + w < 0) continue;

    // Front face: shaded by absolute y, so it recedes into shadow towards the
    // bottom of the screen instead of sitting there as a flat dark slab.
    for (int16_t y = t; y < PLAY_H; y++) cv->drawFastHLine(x, y, w, rockLUT[y]);

    // Chunky brick hint, only near the lit top -- lower down it would just be
    // noise in the shadow. These are anchored to the segment, so they scroll.
    for (int16_t s = x + 8; s < x + w; s += 16)
      cv->drawFastVLine(s, t + 6, min(30, PLAY_H - t - 6), 0x2807);

    // Receding neon lines under the lip, spaced wider as they go down.
    const uint16_t glow[3] = { 0x2A7B, 0x195A, 0x1116 };
    for (uint8_t k = 0; k < 3; k++) {
      const int16_t y = t + 7 + k * k * 5 + k * 4;
      if (y < PLAY_H) cv->drawFastHLine(x, y, w, glow[k]);
    }

    cv->fillRect(x, t, w, 3, C_CYAN);                // the surface itself
    cv->drawFastHLine(x, t + 3, w, 0x03BF);
  }

  // Crates
  for (uint8_t i = 0; i < ncrate; i++) {
    const int16_t x = (int16_t)(crates[i].x - camX), y = crates[i].top;
    if (x > PLAY_W || x + CRATE_S < 0) continue;
    cv->fillRect(x, y, CRATE_S, CRATE_S, C_ORANGE);
    cv->drawRect(x, y, CRATE_S, CRATE_S, C_DARK);
    cv->drawLine(x + 2, y + 2, x + CRATE_S - 3, y + CRATE_S - 3, C_DARK);
    cv->drawLine(x + CRATE_S - 3, y + 2, x + 2, y + CRATE_S - 3, C_DARK);
  }

  // Coins -- a 4-frame spin, faked by squashing the width. Rounded and edged
  // in orange: a plain squashed rectangle just reads as a yellow bar.
  const int16_t cw[4] = { COIN_S, 9, 6, 9 };
  for (uint8_t i = 0; i < ncoin; i++) {
    if (!coins[i].alive) continue;
    const int16_t x = (int16_t)(coins[i].x - camX), y = (int16_t)coins[i].y;
    if (x > PLAY_W || x + COIN_S < 0) continue;
    // Phase from the coin's own world position, so a row of them does not
    // spin in lockstep (which reads as a row of identical bars, not coins).
    if (coins[i].kind == PK_GEM) {
      // A diamond, so it never reads as a big coin.
      const uint8_t p = (millis() / 110) & 3;
      const int16_t h = COIN_S + 2, cxm = x + COIN_S / 2;
      for (int16_t r = 0; r < h; r++) {
        const int16_t half = (r < h / 2) ? (r + 1) : (h - r);
        cv->drawFastHLine(cxm - half, y + r, half * 2, (r < h / 2) ? C_CYAN : 0x03BF);
      }
      cv->fillRect(cxm - 1 + (p == 1 ? 1 : 0), y + 3, 2, 3, C_WHITE);
      continue;
    }
    if (coins[i].kind == PK_HEART) {
      const int16_t b = ((millis() / 140) & 1) ? 1 : 0;   // gentle pulse
      cv->fillRect(x + 1 - b, y + 2 - b, 4 + b * 2, 5 + b, C_MAG);
      cv->fillRect(x + 7 - b, y + 2 - b, 4 + b * 2, 5 + b, C_MAG);
      cv->fillRect(x + 1 - b, y + 5, 10 + b * 2, 3, C_MAG);
      cv->fillRect(x + 3, y + 8, 6, 2, C_MAG);
      cv->fillRect(x + 5, y + 10, 2, 2, C_MAG);
      cv->fillRect(x + 2, y + 3, 2, 2, C_WHITE);
      continue;
    }
    const uint8_t f  = (uint8_t)((millis() / 90 + (uint32_t)(coins[i].x * 0.09f)) & 3);
    const int16_t w  = cw[f], cx = x + (COIN_S - w) / 2;
    cv->fillRoundRect(cx, y, w, COIN_S, 3, C_YELLOW);
    cv->drawRoundRect(cx, y, w, COIN_S, 3, C_ORANGE);
    if (w > 6) cv->fillRect(cx + 2, y + 3, 2, 4, C_WHITE);   // glint
  }

  // Birds
  const uint8_t bf = (millis() / 140) % 2;
  for (uint8_t i = 0; i < nbird; i++) {
    const int16_t x = (int16_t)(birds[i].x - camX);
    const int16_t y = (int16_t)(birds[i].y + sinf(birds[i].phase) * 5.0f);
    if (x > PLAY_W || x + BIRD_W < 0) continue;
    blitSprite(x, y, bf ? &SPR_BIRD_A[0][0] : &SPR_BIRD_B[0][0], 8, 5, 2);
  }

  // Particles
  for (uint8_t i = 0; i < MAX_PART; i++)
    if (parts[i].life)
      cv->fillRect((int16_t)parts[i].x, (int16_t)parts[i].y,
                   parts[i].size, parts[i].size, parts[i].col);

  // Player -- blink while invincible so the hit reads clearly. The title and
  // game-over screens pose their own runner, so skip ours there.
  if (state == ST_PLAY && !(invuln > 0.0f && ((millis() / 70) & 1))) {
    const uint8_t *spr = grounded ? ((millis() / 110) & 1 ? &SPR_RUN_A[0][0]
                                                          : &SPR_RUN_B[0][0])
                                  : &SPR_JUMP[0][0];
    blitSprite(PLAYER_X, (int16_t)py, spr, 8, 10, 2);
  }

  // Shockwave: a ring thrown out from the hit, thinning as it grows.
  if (ringT > 0.0f) {
    const float k = 1.0f - ringT / 0.30f;
    const int16_t r = (int16_t)(6.0f + k * 46.0f);
    const uint16_t c = (k < 0.4f) ? C_WHITE : (k < 0.7f ? C_MAG : 0x7810);
    cv->drawCircle((int16_t)ringX, (int16_t)ringY, r, c);
    if (k < 0.6f) cv->drawCircle((int16_t)ringX, (int16_t)ringY, r - 2, c);
  }

  // The points, floating off the kill.
  if (popT > 0.0f && popText[0]) {
    cv->setTextSize(2);
    const int16_t px = (int16_t)popX - (int16_t)(strlen(popText) * 6);
    cv->setTextColor(C_DARK);   cv->setCursor(px + 1, (int16_t)popY + 1); cv->print(popText);
    cv->setTextColor(popT > 0.4f ? C_WHITE : C_YELLOW);
    cv->setCursor(px, (int16_t)popY); cv->print(popText);
  }

  // Flash, confined to the blast. A full-screen strobe on every stomp is both
  // overwhelming and a real photosensitivity risk in a game aimed at small
  // children -- and localising it reads better anyway, because it points at
  // what just happened instead of washing the whole scene out.
  if (flashT > 0.0f) {
    const int16_t step = (flashT > 0.035f) ? 3 : 5;
    const float   reach = 52.0f;
    const int16_t y0 = max(0, (int)(ringY - reach));
    const int16_t y1 = min((int)PLAY_H, (int)(ringY + reach));
    for (int16_t y = y0; y < y1; y += step) {
      const float d = fabsf((float)y - ringY) / reach;
      const int16_t half = (int16_t)((1.0f - d) * 104.0f);
      if (half > 2) cv->drawFastHLine((int16_t)ringX - half, y, half * 2, 0x9CDF);
    }
  }

  // Banner
  if (banner[0] && millis() < bannerUntil) {
    cv->setTextSize(2);
    const int16_t w = strlen(banner) * 12;
    cv->setTextColor(C_DARK);  cv->setCursor((PLAY_W - w) / 2 + 2, 24); cv->print(banner);
    cv->setTextColor(C_YELLOW);cv->setCursor((PLAY_W - w) / 2,     22); cv->print(banner);
  } else banner[0] = 0;
}

// Arcade-style backdrop, so overlay text never has to compete with the sun,
// a mountain ridge, or a crate that happens to scroll behind a word.
static void panel(int16_t x, int16_t y, int16_t w, int16_t h) {
  cv->fillRoundRect(x, y, w, h, 6, 0x1002);
  cv->drawRoundRect(x, y, w, h, 6, C_MAG);
  cv->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 5, 0x5809);
}

static void drawTitle() {
  cv->setTextSize(4);
  cv->setTextColor(C_MAG);   cv->setCursor(58, 28); cv->print(F("SKOKAN"));
  cv->setTextColor(C_CYAN);  cv->setCursor(56, 26); cv->print(F("SKOKAN"));

  // Kept narrow enough to leave the sun's slits showing beside it.
  panel(20, 64, 200, best ? 60 : 46);
  cv->setTextSize(1);
  cv->setTextColor(C_WHITE);
  cv->setCursor(31, 72);  cv->print(F("PRESS THE BIG BUTTON TO RUN!"));
  cv->setCursor(31, 84);  cv->print(F("TAP = HOP     HOLD = BIG JUMP"));
  cv->setCursor(31, 96);  cv->print(F("PRESS IN THE AIR = DOUBLE JUMP"));
  if (best) {
    cv->setTextColor(C_YELLOW);
    cv->setCursor(31, 110); cv->printf("BEST %lu", (unsigned long)best);
  }
  const int16_t bob = (int16_t)(sinf(millis() / 260.0f) * 5.0f);
  blitSprite(PLAYER_X, TOP_MAX - PLR_H + bob, &SPR_RUN_A[0][0], 8, 10, 2);

  cv->setTextSize(1);
  cv->setTextColor(0x6B5D);
  cv->setCursor(6, PLAY_H - 10);
  cv->print(F("(C) 2026 LETALVOJ.GITHUB.IO"));
}

// The attract overlay. Deliberately much lighter than the title card: the
// logo, a DEMO tag and a blinking prompt, and nothing else -- the whole point
// is that a child watches the bot play and wants a go.
static void drawAttract() {
  cv->setTextSize(3);
  cv->setTextColor(C_DARK); cv->setCursor(74, 10); cv->print(F("SKOKAN"));
  cv->setTextColor(C_CYAN); cv->setCursor(72,  8); cv->print(F("SKOKAN"));

  cv->setTextSize(1);
  cv->setTextColor(C_MAG);
  cv->setCursor(196, 12); cv->print(F("DEMO"));

  if ((millis() / 450) & 1) {
    const int16_t y = PLAY_H - 26;
    cv->setTextColor(C_DARK);  cv->setCursor(74, y + 1); cv->print(F("PRESS TO PLAY!"));
    cv->setTextColor(C_WHITE); cv->setCursor(73, y);     cv->print(F("PRESS TO PLAY!"));
  }
}

static void drawOver() {
  panel(26, 22, 268, 106);

  cv->setTextSize(3);
  cv->setTextColor(C_DARK); cv->setCursor(53, 37); cv->print(F("GAME OVER"));
  cv->setTextColor(C_MAG);  cv->setCursor(50, 34); cv->print(F("GAME OVER"));

  cv->setTextSize(2);
  cv->setTextColor(C_YELLOW);
  cv->setCursor(62, 68);  cv->printf("SCORE %lu", (unsigned long)score);
  cv->setTextSize(1);
  cv->setCursor(62, 90);  cv->printf("%u M    %u COINS", (unsigned)(distance / 32), coinsGot);

  cv->setTextColor(C_WHITE);
  if (millis() - stateSince > 1200 && ((millis() / 400) & 1)) {
    cv->setCursor(62, 106); cv->print(F("PRESS TO PLAY AGAIN"));
  }

  cv->setTextSize(1);
  cv->setTextColor(0x6B5D);
  cv->setCursor(6, PLAY_H - 10);
  cv->print(F("SKOKAN  (C) 2026 LETALVOJ"));
}

// HUD and apron live outside the canvas: they change rarely, so drawing them
// straight to the panel keeps them out of the per-frame SPI budget.
// The apron is the ground's front face, not another floor: same rock and the
// same brick hint as the platforms, so the ground reads as solid all the way
// down instead of looking like a second surface under the first.
static void drawApron() {
  // Picks up exactly where the play band's ground shading left off and keeps
  // darkening, so the ground reads as one solid mass down to the bezel.
  for (int16_t y = APRON_Y; y < SCR_H; y++) {
    const float t = (float)(y - APRON_Y) / (float)(SCR_H - APRON_Y);
    gfx->drawFastHLine(0, y, SCR_W, lerpRGB(8, 0, 20, 0, 0, 6, t));
  }
}

static uint32_t g_lastHudMs = 0;
static uint8_t  g_hudLives = 255, g_hudLevel = 255;

static void drawHud() {
  const uint32_t key = score * 31u + coinsGot * 7u + lives * 3u + level
                     + (uint32_t)(distance / 32);
  if (key == g_lastHud) return;

  // Lives and level must land the instant they change; the score ticks up
  // almost every frame and repainting the bar costs real SPI time, so
  // everything else settles for ~6 Hz, which still reads as live.
  const bool urgent = (lives != g_hudLives) || (level != g_hudLevel);
  const uint32_t now = millis();
  if (!urgent && now - g_lastHudMs < 160) return;

  g_lastHud = key; g_lastHudMs = now;
  g_hudLives = lives; g_hudLevel = level;
  hudDraws++;

  hudCv->fillScreen(C_BLACK);
  hudCv->drawFastHLine(0, HUD_H - 1, SCR_W, C_MAG);
  hudCv->setTextSize(1);

  hudCv->setTextColor(C_CYAN);  hudCv->setCursor(6, 4);  hudCv->print(F("SCORE"));
  hudCv->setTextColor(C_WHITE); hudCv->setCursor(42, 4); hudCv->printf("%06lu", (unsigned long)score);

  hudCv->fillRect(104, 4, 8, 8, C_YELLOW);
  hudCv->fillRect(106, 6, 4, 4, C_ORANGE);
  hudCv->setTextColor(C_YELLOW); hudCv->setCursor(116, 4); hudCv->printf("%03u", coinsGot);

  hudCv->setTextColor(C_CYAN);  hudCv->setCursor(150, 4); hudCv->printf("LV %u", level);
  hudCv->setTextColor(C_WHITE); hudCv->setCursor(196, 4); hudCv->printf("%3u M", (unsigned)(distance / 32));

  for (uint8_t i = 0; i < START_LIVES; i++)
    hudCv->fillRect(258 + i * 20, 4, 14, 12, i < lives ? C_MAG : 0x2104);

  // One transfer, like the play field. No clear-then-redraw on the panel.
  tft->drawRGBBitmap(0, 0, hudCv->getBuffer(), SCR_W, HUD_H);
}

static void render() {
  drawBackground();
  drawWorld();
  if      (state == ST_TITLE) drawTitle();
  else if (state == ST_OVER)  { if (!attract) drawOver(); }
  else if (attract)           drawAttract();

  // One bulk SPI transfer. Must go through `tft`, not `gfx` -- see display.h.
  tft->drawRGBBitmap(0, PLAY_Y, cv->getBuffer(), PLAY_W, PLAY_H);
}

// ---------------------------------------------------------------------------
// Serial
// ---------------------------------------------------------------------------
static void handleSerial() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == 'f') {
      displaySetSpeed(displaySpeed() > 20000000 ? 20000000 : 40000000);
      continue;
    }
    if (c == 'r') { g_rot = (g_rot == 1) ? 3 : 1; gfx->setRotation(g_rot);
                    g_needStatic = true; Serial.printf("[game] rotation %u\n", g_rot);
                    continue; }
    if (displayHandleChar(c)) {   // d / i / s / ? still work
      gfx->setRotation(g_rot);
      g_needStatic = true;
    }
  }
}

#ifdef GAME_HOST
// ---------------------------------------------------------------------------
// Hooks for the macOS harness (host/) only -- never compiled into firmware.
// They let the desktop build autopilot the runner and grab screenshots, so
// the look of the game can be reviewed without a board on the desk.
// ---------------------------------------------------------------------------
void hostFrameDone();

// What is `dx` pixels in front of the runner: 0 safe, 1 gap, 2 crate, 3 bird.
int gameProbe(float dx) {
  const float x = camX + PLAYER_X + dx;
  for (uint8_t i = 0; i < ncrate; i++)
    if (x + PLR_W > crates[i].x && x < crates[i].x + CRATE_S) return 2;
  for (uint8_t i = 0; i < nbird; i++)
    if (x + PLR_W > birds[i].x && x < birds[i].x + BIRD_W) return 3;
  // Narrow window: sampling the runner's full width here would straddle both
  // banks of a small gap and report solid ground over open water.
  return surfaceAt(x + PLR_W / 2 - 1, x + PLR_W / 2 + 1) == NO_GROUND ? 1 : 0;
}
// Walk the live world and count genuinely unclearable configurations. The
// contract is: nothing that FORCES a jump (a gap, a crate) may have a bird
// anywhere inside the jump arc it forces, and no bird may hover over water.
// Run every frame by the harness, so "never impossible" is a measured claim
// rather than an assertion in a comment.
int gameCheckWorld() {
  int bad = 0;
  const float arc = speed * AIRTIME;      // how far a forced jump carries you

  for (uint8_t b = 0; b < nbird; b++) {
    const float bx0 = birds[b].x, bx1 = birds[b].x + BIRD_W;

    if (surfaceAt(bx0, bx1) == NO_GROUND) bad++;          // bird over water

    for (uint8_t c = 0; c < ncrate; c++)
      if (bx1 > crates[c].x - arc && bx0 < crates[c].x + CRATE_S + arc) bad++;

    for (uint8_t i = 1; i < nseg; i++) {
      const float g0 = segs[i - 1].x + segs[i - 1].w, g1 = segs[i].x;
      if (g1 > g0 && bx1 > g0 - arc && bx0 < g1 + arc) bad++;
    }
  }
  return bad;
}
// Hand the controls to the bot at a chosen skill, for measured rollouts.
void gameSetBot(int profile, bool drive) {
  botSkill = BOT_PROFILES[profile % BOT_KINDS];
  botDrive = drive;
  attract  = false;
}
const char *gameBotName(int profile) { return BOT_PROFILES[profile % BOT_KINDS].name; }

void gameStatsReset() {
  memset(statFrames, 0, sizeof statFrames);
  memset(statJumps,  0, sizeof statJumps);
  memset(statCoins,  0, sizeof statCoins);
  memset(statDeath,  0, sizeof statDeath);
  memset(statLevelHist, 0, sizeof statLevelHist);
  memset(statCoinSpawn, 0, sizeof statCoinSpawn);
  statScore = 0;
  statRuns = 0;
}
// Deepest level a run reached. Losing a life DROPS a level, so counting
// level-up events would over-count the same level many times per run.
void gameStatReached(uint8_t lv) { if (lv > statRunMax) statRunMax = lv; }
void gameStatsEndRun() {
  statRuns++;
  statScore += score;
  statLevelHist[statRunMax < STAT_LV ? statRunMax : STAT_LV - 1]++;
  statRunMax = 1;
}
uint32_t gameStatFrames(int lv) { return statFrames[lv]; }
uint32_t gameStatJumps(int lv)  { return statJumps[lv]; }
uint32_t gameStatCoins(int lv)  { return statCoins[lv]; }
uint32_t gameStatDeath(int lv, int c) { return statDeath[lv][c]; }
uint32_t gameStatRuns()         { return statRuns; }
uint32_t gameStatLevelHist(int lv) { return statLevelHist[lv]; }
uint32_t gameStatCoinSpawn(int lv) { return statCoinSpawn[lv]; }
uint32_t gameStatScore()          { return statScore; }
int      gameStatLevels()       { return STAT_LV; }
float    gameDistance()         { return distance; }
void     gameStartNow()         { newGame(); }

int      gameEggKind()  { return egg.active ? (int)egg.kind : -1; }
// True in the frames right after a bird stomp, so the harness can photograph
// the explosion instead of guessing a frame number.
bool     gameStomping() { return ringT > 0.10f; }
bool     gameEggOnScreen() { return egg.active && egg.x > 60.0f && egg.x < PLAY_W - 60.0f; }
bool     gameGrounded() { return grounded; }
int      gameState()    { return (int)state; }
uint8_t  gameLevel()    { return level; }
uint32_t gameScore()    { return score; }
const uint16_t *gamePanel();
#endif

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\n=== mluvitko :: SKOKAN ==="));

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BUTTON_LED, 0);
  ledcWrite(0, 0);

  randomSeed(esp_random());

  displayBegin();
  gfx->setRotation(g_rot);
  synthBegin();

  prefs.begin("mluvitko", false);
  best = prefs.getUInt("best", 0);
  prefs.end();

  cv    = new Canvas(PLAY_W, PLAY_H);
  hudCv = new Canvas(SCR_W, HUD_H);
  buildBackgroundTables();

  // loop() is pinned to core 1 by the Arduino core, and so is the synth task
  // -- worth knowing when chasing frame-time jitter.
  Serial.printf("[game] loop() runs on core %d\n", xPortGetCoreID());

  // The title screen wants terrain to stand on, but not a running game.
  nseg = 0;
  segs[nseg++] = { -80.0f, 480.0f, TOP_MAX };
  segs[nseg++] = { 400.0f, 400.0f, TOP_MAX };
  speed = SPEED_0; level = 1; lives = START_LIVES;
  py = TOP_MAX - PLR_H; vy = 0; grounded = true;
  state = ST_TITLE; stateSince = millis();

  Serial.printf("[game] best score %lu -- press the button\n", (unsigned long)best);
}

void loop() {
  static uint32_t last = 0, fpsT = 0, frames = 0, renderUs = 0, worstUs = 0;

  handleSerial();
  synthTick();

  if (g_needStatic) {
    gfx->fillScreen(C_BLACK);
    drawApron();
    g_lastHud = 0xFFFFFFFF;
    g_hudLives = g_hudLevel = 255;   // force an immediate repaint
    g_needStatic = false;
  }

  const uint32_t now = millis();
  float dt = (last == 0) ? 0.02f : (now - last) / 1000.0f;
  last = now;
  dt = constrain(dt, 0.001f, 0.060f);   // never let a hiccup teleport anyone

  const bool pressed = readButton();

  switch (state) {
    case ST_TITLE:
      camX += 30.0f * dt;               // the world drifts behind the title
      cullAndTop();                     // ...and keeps generating, so it never runs out
      if (pressed) { newGame(); sfxLevel(); }
      else if (now - stateSince > ATTRACT_AFTER_MS) startAttract();
      break;
    case ST_PLAY:
      // The bot drives in attract mode, and on the desktop harness when it is
      // measuring. Either way it goes through tryJump() like a player would.
      if (botDrive) { botThink(dt); holding = botHolding; }
      else if (pressed) tryJump();
      if (attract && pressed) { newGame(); sfxLevel(); }   // a child took over
      else if (freezeT > 0.0f) freezeT -= dt;              // hitstop
      else step(dt);
      break;
    case ST_OVER:
      synthHoldAmp(false);
      // A demo that ended goes quietly back to the menu rather than sitting
      // on a GAME OVER card nobody played for.
      if (attract && now - stateSince > 1500) { attract = botDrive = false;
                                                state = ST_TITLE; stateSince = now; }
      else if (pressed && now - stateSince > 1200) { state = ST_TITLE; stateSince = now; }
      break;
  }

  // The button LED is the runner's heartbeat: steady glow while alive,
  // a bright kick on every jump, angry blinking when the game is over.
  uint8_t duty;
  if (state == ST_OVER)      duty = ((now / 200) & 1) ? 200 : 0;
  else if (state == ST_TITLE)duty = 40 + (uint8_t)(30 * (1 + sinf(now / 400.0f)));
  else                       duty = grounded ? 70 : 230;
  ledcWrite(0, duty);

  const uint32_t t0 = micros();
#ifdef GAME_HOST
  if (!gHeadless) { render(); drawHud(); }
#else
  render();
  drawHud();
#endif
  const uint32_t frameUs = micros() - t0;
  renderUs += frameUs;

#ifdef GAME_HOST
  hostFrameDone();
#endif

  // Frame timing is the thing worth watching here: the play field is blitted
  // in one SPI transfer, so fps is essentially PLAY_W*PLAY_H*2 / SPI clock.
  if (frameUs > worstUs) worstUs = frameUs;
  if (fpsT == 0) fpsT = now;
  else if (++frames >= 40 && now > fpsT) {
    // worst-vs-mean is the number that matters for judder: a steady 17 fps
    // looks far better than 17 fps average with frames twice as long as the
    // rest, which is what preemption by the audio task would look like.
    Serial.printf("[game] %.1f fps  mean %lu us  worst %lu us  hud %u/40  "
                  "state=%u speed=%.0f\n",
                  frames * 1000.0f / (now - fpsT), (unsigned long)(renderUs / frames),
                  (unsigned long)worstUs, hudDraws, (unsigned)state, speed);
    fpsT = now; frames = 0; renderUs = 0; worstUs = 0; hudDraws = 0;
  }
}
