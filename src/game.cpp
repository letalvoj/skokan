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
static const float SPEED_0    = 68.0f;     // px/s at level 1
static const float SPEED_STEP = 1.115f;    // per level
static const float SPEED_MAX  = 235.0f;
static const float LEVEL_DIST = 900.0f;    // px of ground per level

static const uint8_t START_LIVES = 3;

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------
struct Seg   { float x, w; int16_t top; };
struct Coin  { float x, y; bool alive; };
struct Crate { float x; int16_t top; };
struct Bird  { float x, y, phase; bool alive; };
struct Part  { float x, y, vx, vy; uint8_t life; uint16_t col; };

static const uint8_t MAX_SEG = 14, MAX_COIN = 64, MAX_CRATE = 16;
static const uint8_t MAX_BIRD = 6, MAX_PART = 28;

static Seg   segs[MAX_SEG];    static uint8_t nseg;
static Coin  coins[MAX_COIN];  static uint8_t ncoin;
static Crate crates[MAX_CRATE];static uint8_t ncrate;
static Bird  birds[MAX_BIRD];  static uint8_t nbird;
static Part  parts[MAX_PART];

static float    camX;              // world x at the left edge of the screen
static float    speed;             // px/s
static uint8_t  level;
static uint32_t score, best;
static uint16_t coinsGot;
static uint8_t  lives;
static float    distance;          // px run this game
static float    scoreAcc;          // sub-point remainder of the distance score
static bool     prevHard;          // last segment was a hard one

// Player
static float   py, vy;             // band-local y of the sprite top
static bool    grounded, usedDouble, holding;
static float   coyote, buffered, invuln;
static uint8_t birdCombo;

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

static Preferences prefs;
static uint8_t  g_rot = 1;            // landscape; 'r' flips to the other one
static bool     g_needStatic = true;  // HUD + apron need a repaint
static uint32_t g_lastHud = 0xFFFFFFFF;

// Precomputed background
static uint16_t skyLUT[PLAY_H];
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
static const float SAFETY = 0.58f;

static float maxGapFor(float rise) {
  const float reach   = speed * AIRTIME * SAFETY;
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
  coins[ncoin++] = { x, y, true };
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

static void addSegment() {
  const Seg &p = segs[nseg - 1];
  float gap = 0.0f;
  int16_t top = p.top;

  if (prevHard) {
    // Never two hard things in a row: give them a wide flat landing strip.
    prevHard = false;
  } else {
    const float rise = (random(100) < 45) ? frnd(4.0f, maxRise()) : 0.0f;
    const float drop = (rise == 0.0f && random(100) < 40) ? frnd(6.0f, 40.0f) : 0.0f;
    top = (int16_t)constrain((float)p.top - rise + drop, (float)TOP_MIN, (float)TOP_MAX);

    const float actualRise = (float)(p.top - top);          // >0 means step up
    if (random(100) < (level < 2 ? 35 : 70))
      gap = frnd(18.0f, maxGapFor(actualRise));

    prevHard = (gap > maxGapFor(actualRise) * 0.7f) || (actualRise > maxRise() * 0.7f);
  }

  // Wide enough to land, breathe and take off again at the current speed.
  const float w = fmaxf(90.0f, speed * 0.90f) + frnd(0.0f, 90.0f);
  if (nseg >= MAX_SEG) return;
  segs[nseg++] = { p.x + p.w + gap, w, top };
  const Seg &s = segs[nseg - 1];

  // Coins over the gap, following the jump that clears it. The arc starts a
  // little BEFORE the lip: jumping slightly early is the safe way to clear a
  // gap, so the coins should reward that rather than punish it.
  if (gap > 0.0f) coinArc(p.x + p.w - 26.0f, (float)p.top, 4);

  // Crates: something to hop over. Never near an edge, where the player is
  // busy landing, and never before level 2.
  if (level >= 2 && s.w > 118.0f && random(100) < 55) {
    const float cx = s.x + frnd(48.0f, s.w - 58.0f);
    if (ncrate < MAX_CRATE) {
      crates[ncrate++] = { cx, (int16_t)(s.top - CRATE_S) };
      coinArc(cx - 34.0f, (float)s.top, 3);
    }
  } else if (random(100) < 45) {
    // Otherwise a low row of coins: a free hop, and a hint that up is good.
    const float cx = s.x + frnd(40.0f, fmaxf(45.0f, s.w - 60.0f));
    for (uint8_t i = 0; i < 3; i++)
      addCoin(cx + i * 18.0f, (float)s.top - 34.0f);
  }
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
      parts[i] = { x, y, frnd(-70, 70), frnd(-110, -20), (uint8_t)random(7, 13), col };
      made++;
    }
}

// ---------------------------------------------------------------------------
// Reset / start
// ---------------------------------------------------------------------------
static void newGame() {
  nseg = ncoin = ncrate = nbird = 0;
  for (uint8_t i = 0; i < MAX_PART; i++) parts[i].life = 0;

  camX = 0; speed = SPEED_0; level = 1; score = 0; scoreAcc = 0; coinsGot = 0;
  lives = START_LIVES; distance = 0; prevHard = false; birdCombo = 0;
  invuln = 0; coyote = 0; buffered = 0; usedDouble = false; holding = false;

  // Three flat, gapless segments to start on -- nothing to fail at yet, and a
  // low row of coins so the very first thing that happens is a small win.
  segs[nseg++] = { -80.0f, 460.0f, 138 };
  segs[nseg++] = { 380.0f, 300.0f, 138 };
  segs[nseg++] = { 680.0f, 300.0f, 138 };
  py = 138 - PLR_H; vy = 0; grounded = true;
  for (uint8_t i = 0; i < 4; i++) addCoin(250.0f + i * 20.0f, 138.0f - 32.0f);
  cullAndTop();

  g_lastHud = 0xFFFFFFFF;
  banner[0] = 0;
  state = ST_PLAY; stateSince = millis();
  synthHoldAmp(true);
  Serial.printf("[game] start: jump height %.0f px, airtime %.2f s, "
                "reach at level 1 = %.0f px\n", JUMP_H, AIRTIME, SPEED_0 * AIRTIME);
}

static void hurt(const char *why) {
  if (invuln > 0.0f) return;
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

static void tryJump() {
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

  const uint8_t want = (uint8_t)(distance / LEVEL_DIST) + 1;
  if (want > level) {
    level = want;
    speed = fminf(SPEED_0 * powf(SPEED_STEP, level - 1), SPEED_MAX);
    snprintf(banner, sizeof(banner), "LEVEL %u", level);
    bannerUntil = millis() + 1200;
    sfxLevel();
    Serial.printf("[game] level %u, speed %.0f px/s, max gap %.0f px\n",
                  level, speed, maxGapFor(0));
  }
  cullAndTop();

  // --- vertical ---
  const float g = (vy < 0.0f) ? (holding ? G_UP_HELD : G_UP_FREE) : G_DOWN;
  vy = fminf(vy + g * dt, VY_MAX);
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

  const bool wasGrounded = grounded;
  grounded = false;
  // No prevBottom test on purpose. Platforms are solid columns with no
  // undersides to jump through, so "feet at or below the surface while
  // falling" can only mean landing -- or having clipped the side of a step,
  // which we forgive by snapping up rather than burying the player in rock.
  if (sup != NO_GROUND && vy >= 0.0f && py + PLR_H >= (float)sup) {
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

  // --- fell down a pit ---
  if (py > PLAY_H + 10.0f) {
    invuln = 0;
    hurt("pit");
    if (state == ST_OVER) return;
    // Put them back on the next real ground ahead, mid-air, still moving.
    for (uint8_t i = 0; i < nseg; i++)
      if (segs[i].x > camX + PLAYER_X) {
        camX = segs[i].x - PLAYER_X + 8.0f;
        py   = segs[i].top - PLR_H - 40.0f;
        break;
      }
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
  if (level >= 3 && nbird < MAX_BIRD && random(1000) < 7) {
    const float bx = camX + PLAY_W + 20.0f;
    const int16_t s = surfaceAt(bx, bx + BIRD_W);
    const float base = (s == NO_GROUND) ? (float)TOP_MAX : (float)s;
    birds[nbird++] = { bx, base - frnd(40.0f, 76.0f), frnd(0, 6.28f), true };
  }
  for (uint8_t i = 0; i < nbird; i++) {
    Bird &b = birds[i];
    b.x     -= (speed * 0.45f) * dt;      // they fly towards you
    b.phase += 6.0f * dt;
    const float by = b.y + sinf(b.phase) * 7.0f;
    if (wx + PLR_W - 3 > b.x && wx + 3 < b.x + BIRD_W &&
        py + PLR_H > by && py < by + BIRD_H) {
      if (vy > 40.0f && py + PLR_H < by + BIRD_H * 0.8f) {
        b.alive = false;
        vy = STOMP_V0; usedDouble = false;
        birdCombo++;
        score += 50 * birdCombo;
        puff(b.x - camX + BIRD_W / 2, by, C_MAG, 8);   // particles live in screen space
        sfxStomp();
        if (birdCombo >= 3) {
          snprintf(banner, sizeof(banner), "TRIPLE!");
          bannerUntil = millis() + 1100;
        }
      } else hurt("bird");
    }
  }

  // --- coins ---
  for (uint8_t i = 0; i < ncoin; i++) {
    Coin &c = coins[i];
    if (!c.alive) continue;
    if (wx + PLR_W - 2 > c.x && wx + 2 < c.x + COIN_S &&
        py + PLR_H > c.y && py < c.y + COIN_S) {
      c.alive = false; coinsGot++; score += 10;
      puff(c.x - camX + COIN_S / 2, c.y + COIN_S / 2, C_YELLOW, 4);
      sfxCoin();
    }
  }

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
    for (int16_t y = GRID_TOP; y < PLAY_H; y++) {
      const float t = (float)(y - GRID_TOP) / (PLAY_H - GRID_TOP);
      cv->drawPixel((int16_t)(xh + (xb - xh) * t), y, C_GRID);
    }
  }
}

static void drawWorld() {
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
    const int16_t y = (int16_t)(birds[i].y + sinf(birds[i].phase) * 7.0f);
    if (x > PLAY_W || x + BIRD_W < 0) continue;
    blitSprite(x, y, bf ? &SPR_BIRD_A[0][0] : &SPR_BIRD_B[0][0], 8, 5, 2);
  }

  // Particles
  for (uint8_t i = 0; i < MAX_PART; i++)
    if (parts[i].life)
      cv->fillRect((int16_t)parts[i].x, (int16_t)parts[i].y, 3, 3, parts[i].col);

  // Player -- blink while invincible so the hit reads clearly. The title and
  // game-over screens pose their own runner, so skip ours there.
  if (state == ST_PLAY && !(invuln > 0.0f && ((millis() / 70) & 1))) {
    const uint8_t *spr = grounded ? ((millis() / 110) & 1 ? &SPR_RUN_A[0][0]
                                                          : &SPR_RUN_B[0][0])
                                  : &SPR_JUMP[0][0];
    blitSprite(PLAYER_X, (int16_t)py, spr, 8, 10, 2);
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

static void drawHud() {
  const uint32_t key = score * 31u + coinsGot * 7u + lives * 3u + level;
  if (key == g_lastHud) return;
  g_lastHud = key;

  gfx->fillRect(0, 0, SCR_W, HUD_H, C_BLACK);
  gfx->drawFastHLine(0, HUD_H - 1, SCR_W, C_MAG);
  gfx->setTextSize(1);

  gfx->setTextColor(C_CYAN);   gfx->setCursor(6, 4);   gfx->print(F("SCORE"));
  gfx->setTextColor(C_WHITE);  gfx->setCursor(42, 4);  gfx->printf("%06lu", (unsigned long)score);

  gfx->fillRect(104, 4, 8, 8, C_YELLOW);
  gfx->fillRect(106, 6, 4, 4, C_ORANGE);
  gfx->setTextColor(C_YELLOW); gfx->setCursor(116, 4); gfx->printf("%03u", coinsGot);

  gfx->setTextColor(C_CYAN);   gfx->setCursor(150, 4); gfx->printf("LV %u", level);
  gfx->setTextColor(C_WHITE);  gfx->setCursor(196, 4); gfx->printf("%3u M", (unsigned)(distance / 32));

  for (uint8_t i = 0; i < START_LIVES; i++)
    gfx->fillRect(258 + i * 20, 4, 14, 12, i < lives ? C_MAG : 0x2104);
}

static void render() {
  drawBackground();
  if (state == ST_TITLE)     { drawWorld(); drawTitle(); }
  else if (state == ST_OVER) { drawWorld(); drawOver();  }
  else                         drawWorld();

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
  return surfaceAt(x, x + PLR_W) == NO_GROUND ? 1 : 0;
}
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

  cv = new Canvas(PLAY_W, PLAY_H);
  buildBackgroundTables();

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
  static uint32_t last = 0, fpsT = 0, frames = 0, renderUs = 0;

  handleSerial();
  synthTick();

  if (g_needStatic) {
    gfx->fillScreen(C_BLACK);
    drawApron();
    g_lastHud = 0xFFFFFFFF;
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
      break;
    case ST_PLAY:
      if (pressed) tryJump();
      step(dt);
      break;
    case ST_OVER:
      synthHoldAmp(false);
      if (pressed && now - stateSince > 1200) { state = ST_TITLE; stateSince = now; }
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
  render();
  renderUs += micros() - t0;
  drawHud();

#ifdef GAME_HOST
  hostFrameDone();
#endif

  // Frame timing is the thing worth watching here: the play field is blitted
  // in one SPI transfer, so fps is essentially PLAY_W*PLAY_H*2 / SPI clock.
  if (fpsT == 0) fpsT = now;
  else if (++frames >= 40 && now > fpsT) {
    Serial.printf("[game] %.1f fps  (render+blit %lu us)  state=%u speed=%.0f\n",
                  frames * 1000.0f / (now - fpsT), (unsigned long)(renderUs / frames),
                  (unsigned)state, speed);
    fpsT = now; frames = 0; renderUs = 0;
  }
}
