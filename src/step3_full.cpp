// esp32-mluvitko -- STEP 3: everything. Face + button + speaker + microphone.
//
// The arcade button is now a RECORD button: its LED means "listening". While
// the LED is on the mic runs and drives an 80s-radio segmented spectrum
// analyser under the face. While it is off the mic's I2S peripheral is
// stopped outright, so its clock lines sit idle next to the display's SPI.

#include <Arduino.h>
#include <math.h>
#include "display.h"
#include "synth.h"
#include "mic.h"
#include "pins.h"

static const uint16_t BLACK   = 0x0000;
static const uint16_t WHITE   = 0xFFFF;
static const uint16_t GREY    = 0x8410;
static const uint16_t DIM     = 0x2104;   // unlit segment
static const uint16_t YELLOW  = 0xFFE0;
static const uint16_t CYAN    = 0x07FF;
static const uint16_t PINK    = 0xFB56;
static const uint16_t ORANGE  = 0xFD20;
static const uint16_t GREEN   = 0x07E0;
static const uint16_t RED     = 0xF800;

enum Mood : uint8_t { MOOD_HAPPY = 0, MOOD_WINK, MOOD_COOL, MOOD_SURPRISED };
static const char *MOOD_NAMES[] = { "happy", "wink", "cool", "wow" };
static const uint8_t MOOD_COUNT = 4;

static Mood     mood       = MOOD_HAPPY;
static uint32_t pressCount = 0;
static bool     listening  = false;      // == the LED state

// ---- layout ------------------------------------------------------------
static const int16_t HEADER_H = 26, NOTE_H = 18, SPEC_H = 36, FOOTER_H = 24;
static const uint8_t NBARS = 16, NSEG = 8;
static const int16_t SEG_H = 3, SEG_GAP = 1, BAR_GAP = 2;

static int16_t CX, CY, R, EYE_L, EYE_R_X, EYE_Y, MOUTH_Y;
static int16_t NOTE_Y, SPEC_TOP, FOOTER_Y, BAR_W, BAR_X0;

static int16_t  lastMouthH = -1;
static uint8_t  lastMidiDrawn = 255;
static uint32_t nextBlink = 0;

static float   bars[NBARS], smooth[NBARS];
static uint8_t barLevel[NBARS], barPeak[NBARS], prevLevel[NBARS], prevPeak[NBARS];
static uint32_t lastPeakDrop = 0;

static bool     lastStable = true, lastReading = true;
static uint32_t lastChange = 0;

static const char *NOTE_NAMES[12] =
  {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

static void computeLayout() {
  const int16_t w = scrW(), h = scrH();
  const int16_t faceH = h - HEADER_H - NOTE_H - SPEC_H - FOOTER_H;

  CX = w / 2;
  CY = HEADER_H + faceH / 2;
  R  = min<int16_t>(w / 2 - 12, faceH / 2 - 4);

  EYE_L   = CX - R / 3;
  EYE_R_X = CX + R / 3;
  EYE_Y   = CY - R / 3;
  MOUTH_Y = CY + R / 4;

  NOTE_Y   = HEADER_H + faceH;
  SPEC_TOP = NOTE_Y + NOTE_H;
  FOOTER_Y = h - FOOTER_H;

  BAR_W  = (w - BAR_GAP * (NBARS + 1)) / NBARS;
  BAR_X0 = (w - (BAR_W + BAR_GAP) * NBARS + BAR_GAP) / 2;

  lastMouthH = -1;
  lastMidiDrawn = 255;
  for (uint8_t i = 0; i < NBARS; i++) {
    smooth[i] = 0.0f;
    barLevel[i] = barPeak[i] = 0;
    prevLevel[i] = prevPeak[i] = 255;
  }
}

// ---- face --------------------------------------------------------------
static void drawEyes(bool closed) {
  const int16_t er = R / 7;
  gfx->fillRect(EYE_L - er - 14, EYE_Y - er - 12,
                (EYE_R_X - EYE_L) + 2 * er + 28, 2 * er + 24, YELLOW);

  if (mood == MOOD_COOL) {
    const int16_t lw = er * 2 + 10, lh = er * 2;
    gfx->fillRoundRect(EYE_L - lw / 2, EYE_Y - lh / 2, lw, lh, 4, BLACK);
    gfx->fillRoundRect(EYE_R_X - lw / 2, EYE_Y - lh / 2, lw, lh, 4, BLACK);
    gfx->fillRect(EYE_L + lw / 2, EYE_Y - 2, EYE_R_X - EYE_L - lw, 4, BLACK);
    return;
  }

  const int16_t er2 = (mood == MOOD_SURPRISED) ? er + 4 : er;
  const bool rightShut = closed || (mood == MOOD_WINK);

  if (closed) gfx->drawFastHLine(EYE_L - er2, EYE_Y, er2 * 2, BLACK);
  else {
    gfx->fillCircle(EYE_L, EYE_Y, er2, BLACK);
    gfx->fillCircle(EYE_L + er2 / 3, EYE_Y - er2 / 3, er2 / 3, WHITE);
  }
  if (rightShut) {
    gfx->drawFastHLine(EYE_R_X - er2, EYE_Y, er2 * 2, BLACK);
    gfx->drawFastHLine(EYE_R_X - er2 + 2, EYE_Y - 1, er2 * 2 - 4, BLACK);
  } else {
    gfx->fillCircle(EYE_R_X, EYE_Y, er2, BLACK);
    gfx->fillCircle(EYE_R_X + er2 / 3, EYE_Y - er2 / 3, er2 / 3, WHITE);
  }
}

static void drawMouth(float openness) {
  const int16_t mw = R;
  const int16_t h  = 4 + (int16_t)(openness * (R * 0.55f));
  if (h == lastMouthH) return;
  lastMouthH = h;

  gfx->fillRect(CX - mw / 2 - 6, MOUTH_Y - 8, mw + 12, (int16_t)(R * 0.62f) + 20, YELLOW);

  if (openness < 0.12f) {
    for (int deg = 205; deg <= 335; deg += 4) {
      const float rad = deg * DEG_TO_RAD;
      gfx->fillCircle(CX + (int16_t)(cosf(rad) * (R * 0.55f)),
                      CY - (int16_t)(sinf(rad) * (R * 0.55f)),
                      max(2, R / 22), BLACK);
    }
  } else {
    gfx->fillRoundRect(CX - mw / 2, MOUTH_Y, mw, h, min<int16_t>(h / 2, 14), BLACK);
    if (h > 18)
      gfx->fillRoundRect(CX - mw / 5, MOUTH_Y + h - h / 3, mw * 2 / 5, h / 3, 5, PINK);
  }
}

static void drawFaceBase() {
  gfx->fillCircle(CX, CY, R, YELLOW);
  drawEyes(false);
  lastMouthH = -1;
  drawMouth(0.0f);
}

// ---- spectrum ----------------------------------------------------------
static uint16_t segColor(uint8_t j) {
  if (j >= NSEG - 1) return RED;
  if (j >= NSEG - 3) return YELLOW;
  return GREEN;
}

static void drawBar(uint8_t b) {
  const int16_t x = BAR_X0 + b * (BAR_W + BAR_GAP);
  for (uint8_t j = 0; j < NSEG; j++) {
    const int16_t y = SPEC_TOP + SPEC_H - (j + 1) * (SEG_H + SEG_GAP);
    uint16_t c = DIM;
    if (j < barLevel[b])       c = segColor(j);
    else if (j == barPeak[b])  c = WHITE;      // classic peak-hold cap
    gfx->fillRect(x, y, BAR_W, SEG_H, c);
  }
  prevLevel[b] = barLevel[b];
  prevPeak[b]  = barPeak[b];
}

static void drawSpectrum(bool force) {
  for (uint8_t b = 0; b < NBARS; b++)
    if (force || barLevel[b] != prevLevel[b] || barPeak[b] != prevPeak[b])
      drawBar(b);
}

static void clearSpectrum() {
  gfx->fillRect(0, SPEC_TOP, scrW(), SPEC_H, BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(GREY, BLACK);
  gfx->setCursor(BAR_X0, SPEC_TOP + SPEC_H / 2 - 4);
  gfx->print(F("press to listen"));
  for (uint8_t i = 0; i < NBARS; i++) { prevLevel[i] = prevPeak[i] = 255; }
}

// ---- chrome ------------------------------------------------------------
static void drawHeader() {
  gfx->fillRect(0, 0, scrW(), HEADER_H, BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(CYAN, BLACK);
  gfx->setCursor(10, 5);
  gfx->print(F("MLUVITKO"));
  if (listening) {
    gfx->fillCircle(scrW() - 18, 13, 6, RED);
    gfx->setTextSize(1);
    gfx->setTextColor(RED, BLACK);
    gfx->setCursor(scrW() - 60, 10);
    gfx->print(F("REC"));
  }
}

static void drawFooter() {
  gfx->fillRect(0, FOOTER_Y, scrW(), FOOTER_H, BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(listening ? ORANGE : GREY, BLACK);
  gfx->setCursor(10, FOOTER_Y + 4);
  gfx->printf("LED %s", listening ? "ON" : "off");
  gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(10, FOOTER_Y + 14);
  gfx->printf("%s   presses: %lu", MOOD_NAMES[mood], (unsigned long)pressCount);
}

static void drawNote(uint8_t midi) {
  if (midi == lastMidiDrawn) return;
  lastMidiDrawn = midi;
  gfx->fillRect(0, NOTE_Y, scrW(), NOTE_H, BLACK);
  if (!midi) return;
  gfx->setTextSize(2);
  gfx->setTextColor(YELLOW, BLACK);
  const char *n = NOTE_NAMES[midi % 12];
  gfx->setCursor(CX - (strlen(n) + 1) * 6, NOTE_Y);
  gfx->printf("%s%d", n, midi / 12 - 1);
}

static void drawAll() {
  gfx->fillScreen(BLACK);
  drawHeader();
  drawFaceBase();
  drawNote(0);
  if (listening) drawSpectrum(true); else clearSpectrum();
  drawFooter();
}

// ---- button ------------------------------------------------------------
static void onPress() {
  pressCount++;
  listening = !listening;
  mood = (Mood)((mood + 1) % MOOD_COUNT);

  Serial.printf("[button] press #%lu -> %s, mood %s\n", (unsigned long)pressCount,
                listening ? "LISTENING" : "idle", MOOD_NAMES[mood]);

  if (listening) { micStart(); drawSpectrum(true); }
  else           { micStop();  clearSpectrum(); }

  drawEyes(false);
  drawHeader();
  drawFooter();
  synthPlay(0xFF);
}

static void pollButton() {
  const bool reading = digitalRead(PIN_BUTTON);
  if (reading != lastReading) { lastReading = reading; lastChange = millis(); }
  if (millis() - lastChange > 25 && reading != lastStable) {
    lastStable = reading;
    if (!reading) onPress();
  }
}

// Our own keys first, then the display calibration ones.
static bool handleSerial() {
  if (!Serial.available()) return false;
  const char c = (char)Serial.read();
  switch (c) {
    case '+': micAdjustMargin(+2.0f); return false;   // less sensitive
    case '-': micAdjustMargin(-2.0f); return false;   // more sensitive
    case ']': micAdjustSpan(+4.0f);   return false;   // harder to max out
    case '[': micAdjustSpan(-4.0f);   return false;   // easier to max out
    default: return displayHandleChar(c);
  }
}

static void updateLed() {
  uint8_t duty = 0;
  if (listening) {
    const float t = (millis() % 2400) / 2400.0f;
    duty = (uint8_t)(90 + 165 * (0.5f - 0.5f * cosf(t * 2.0f * PI)));
  }
  ledcWrite(0, duty);
}

void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  delay(150);

  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F("  esp32-mluvitko -- STEP 3: the whole thing"));
  Serial.println(F("=============================================="));
  Serial.printf("chip %s rev %d @ %lu MHz, psram %lu MB\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                (unsigned long)getCpuFrequencyMhz(),
                (unsigned long)(ESP.getPsramSize() >> 20));

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BUTTON_LED, 0);
  ledcWrite(0, 0);
  Serial.printf("[button] pin %d (pull-up), LED pin %d\n", PIN_BUTTON, PIN_BUTTON_LED);

  synthBegin();
  micBegin();                 // installed but stopped
  displayBegin();
  computeLayout();
  drawAll();

  Serial.println(F("ready -- press the button to start listening"));
  Serial.println(F("=============================================="));
}

void loop() {
  static uint32_t lastReport = 0;
  static float rms = 0.0f;

  if (handleSerial()) { computeLayout(); drawAll(); }

  if (listening && micReadBars(bars, NBARS, &rms)) {
    for (uint8_t b = 0; b < NBARS; b++) {
      // Jump up instantly, fall back gently. Instant in both directions looks
      // like static; the slow fall is what makes it read as a VU meter.
      smooth[b] = max(bars[b], smooth[b] - 0.05f);
      const uint8_t lvl = (uint8_t)(smooth[b] * NSEG + 0.5f);
      barLevel[b] = min<uint8_t>(lvl, NSEG);
      if (barLevel[b] > barPeak[b]) barPeak[b] = barLevel[b];
    }
    // Peak caps fall slowly -- that lag is what makes it read as 80s hi-fi.
    if (millis() - lastPeakDrop > 120) {
      lastPeakDrop = millis();
      for (uint8_t b = 0; b < NBARS; b++) if (barPeak[b]) barPeak[b]--;
    }
    drawSpectrum(false);
  }

  // Singing takes priority over listening for the mouth; otherwise the mouth
  // follows the room's loudness, so it talks back at you.
  const uint8_t midi = g_currentMidi;
  if (midi) {
    drawMouth(constrain((midi - 55) / 36.0f, 0.15f, 1.0f));
  } else if (listening) {
    drawMouth(constrain(rms * 1.4f, 0.0f, 1.0f));
  } else {
    drawMouth(0.0f);
    if (millis() > nextBlink) {
      nextBlink = millis() + random(2400, 5200);
      drawEyes(true); delay(120); drawEyes(false);
    }
  }
  drawNote(midi);

  pollButton();
  updateLed();
  synthTick();

  if (millis() - lastReport > 5000) {
    lastReport = millis();
    Serial.printf("[loop] heap %lu  mic %s  rms %.2f  mood %s  presses %lu\n",
                  (unsigned long)ESP.getFreeHeap(), micRunning() ? "on" : "off",
                  rms, MOOD_NAMES[mood], (unsigned long)pressCount);
  }
}
