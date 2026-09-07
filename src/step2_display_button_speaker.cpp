// esp32-mluvitko -- STEP 2: display + button + speaker. No microphone.
//
// Press the arcade button: the LED toggles, a jingle plays, and the face
// changes mood and sings along -- its mouth opens wider for higher notes,
// and the note name appears under the face.

#include <Arduino.h>
#include <math.h>
#include "display.h"
#include "synth.h"
#include "pins.h"

static const uint16_t BLACK  = 0x0000;
static const uint16_t WHITE  = 0xFFFF;
static const uint16_t GREY   = 0x8410;
static const uint16_t YELLOW = 0xFFE0;
static const uint16_t CYAN   = 0x07FF;
static const uint16_t PINK   = 0xFB56;
static const uint16_t ORANGE = 0xFD20;

enum Mood : uint8_t { MOOD_HAPPY = 0, MOOD_WINK, MOOD_COOL, MOOD_SURPRISED };
static const char *MOOD_NAMES[] = { "happy", "wink", "cool", "wow" };
static const uint8_t MOOD_COUNT = 4;

static Mood     mood       = MOOD_HAPPY;
static uint32_t pressCount = 0;
static bool     ledOn      = false;

// Face geometry (portrait 240x320).
static int16_t CX, CY, R, EYE_L, EYE_R_X, EYE_Y, MOUTH_Y;

static int16_t lastMouthH = -1;
static uint8_t lastMidiDrawn = 255;
static uint32_t nextBlink = 0;

// ---- button ------------------------------------------------------------
static bool     lastStable = true, lastReading = true;
static uint32_t lastChange = 0;

static const char *NOTE_NAMES[12] =
  {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

static void computeLayout() {
  const int16_t w = scrW(), h = scrH();
  CX = w / 2;
  CY = h / 2 + 6;
  R  = min(w, h) / 2 - 26;
  EYE_L   = CX - R / 3;
  EYE_R_X = CX + R / 3;
  EYE_Y   = CY - R / 3;
  MOUTH_Y = CY + R / 4;
  lastMouthH    = -1;
  lastMidiDrawn = 255;
}

static void drawEyes(bool closed) {
  const int16_t er = R / 7;

  // Wipe the whole eye band back to face colour first.
  gfx->fillRect(EYE_L - er - 12, EYE_Y - er - 10,
                (EYE_R_X - EYE_L) + 2 * er + 24, 2 * er + 20, YELLOW);

  if (mood == MOOD_COOL) {
    // Sunglasses: two lenses and a bridge.
    const int16_t lw = er * 2 + 10, lh = er * 2;
    gfx->fillRoundRect(EYE_L - lw / 2, EYE_Y - lh / 2, lw, lh, 4, BLACK);
    gfx->fillRoundRect(EYE_R_X - lw / 2, EYE_Y - lh / 2, lw, lh, 4, BLACK);
    gfx->fillRect(EYE_L + lw / 2, EYE_Y - 2, EYE_R_X - EYE_L - lw, 4, BLACK);
    return;
  }

  const bool leftShut  = closed;
  const bool rightShut = closed || (mood == MOOD_WINK);
  const int16_t bigger = (mood == MOOD_SURPRISED) ? er + 4 : er;

  if (leftShut) gfx->drawFastHLine(EYE_L - bigger, EYE_Y, bigger * 2, BLACK);
  else {
    gfx->fillCircle(EYE_L, EYE_Y, bigger, BLACK);
    gfx->fillCircle(EYE_L + bigger / 3, EYE_Y - bigger / 3, bigger / 3, WHITE);
  }

  if (rightShut) {
    // A wink is a curve, not a flat line -- flat reads as "asleep".
    gfx->drawFastHLine(EYE_R_X - bigger, EYE_Y, bigger * 2, BLACK);
    gfx->drawFastHLine(EYE_R_X - bigger + 2, EYE_Y - 1, bigger * 2 - 4, BLACK);
  } else {
    gfx->fillCircle(EYE_R_X, EYE_Y, bigger, BLACK);
    gfx->fillCircle(EYE_R_X + bigger / 3, EYE_Y - bigger / 3, bigger / 3, WHITE);
  }
}

// openness 0..1 -- 0 is a closed smile, 1 is a wide singing mouth.
static void drawMouth(float openness) {
  const int16_t mw = R;
  const int16_t h  = 4 + (int16_t)(openness * (R * 0.55f));
  if (h == lastMouthH) return;
  lastMouthH = h;

  const int16_t bandY = MOUTH_Y - 8;
  const int16_t bandH = (int16_t)(R * 0.62f) + 20;
  gfx->fillRect(CX - mw / 2 - 6, bandY, mw + 12, bandH, YELLOW);

  if (openness < 0.12f) {
    // Closed: a smile swept as a row of small circles.
    for (int deg = 205; deg <= 335; deg += 4) {
      const float rad = deg * DEG_TO_RAD;
      gfx->fillCircle(CX + (int16_t)(cosf(rad) * (R * 0.55f)),
                      CY - (int16_t)(sinf(rad) * (R * 0.55f)),
                      max(2, R / 22), BLACK);
    }
  } else {
    gfx->fillRoundRect(CX - mw / 2, MOUTH_Y, mw, h, min<int16_t>(h / 2, 14), BLACK);
    if (h > 18)   // a tongue, once the mouth is open enough to show one
      gfx->fillRoundRect(CX - mw / 5, MOUTH_Y + h - h / 3, mw * 2 / 5, h / 3, 5, PINK);
  }
}

static void drawFaceBase() {
  gfx->fillCircle(CX, CY, R, YELLOW);
  drawEyes(false);
  lastMouthH = -1;
  drawMouth(0.0f);
}

static void drawHeader() {
  gfx->fillRect(0, 0, scrW(), 26, BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(CYAN, BLACK);
  gfx->setCursor(10, 6);
  gfx->print(F("MLUVITKO"));
}

static void drawFooter() {
  const int16_t y = scrH() - 26;
  gfx->fillRect(0, y, scrW(), 26, BLACK);
  gfx->setTextSize(1);

  gfx->setTextColor(ledOn ? ORANGE : GREY, BLACK);
  gfx->setCursor(10, y + 4);
  gfx->printf("LED %s", ledOn ? "ON" : "off");

  gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(10, y + 15);
  gfx->printf("%s   presses: %lu", MOOD_NAMES[mood], (unsigned long)pressCount);
}

// The note being sung, printed just under the face.
static void drawNote(uint8_t midi) {
  if (midi == lastMidiDrawn) return;
  lastMidiDrawn = midi;

  const int16_t y = CY + R + 4;
  gfx->fillRect(0, y, scrW(), 18, BLACK);
  if (!midi) return;

  gfx->setTextSize(2);
  gfx->setTextColor(YELLOW, BLACK);
  const char *n = NOTE_NAMES[midi % 12];
  gfx->setCursor(CX - (strlen(n) + 1) * 6, y);
  gfx->printf("%s%d", n, midi / 12 - 1);
}

static void drawAll() {
  gfx->fillScreen(BLACK);
  drawHeader();
  drawFaceBase();
  drawFooter();
  lastMidiDrawn = 255;
  drawNote(0);
}

static void onPress() {
  pressCount++;
  ledOn = !ledOn;
  mood  = (Mood)((mood + 1) % MOOD_COUNT);
  Serial.printf("[button] press #%lu -> LED %s, mood %s\n",
                (unsigned long)pressCount, ledOn ? "ON" : "off", MOOD_NAMES[mood]);
  drawEyes(false);
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

static void updateLed() {
  uint8_t duty = 0;
  if (ledOn) {
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
  Serial.println(F("  esp32-mluvitko -- STEP 2: display + button"));
  Serial.println(F("  (microphone is NOT running)"));
  Serial.println(F("=============================================="));
  Serial.printf("chip %s rev %d @ %lu MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), (unsigned long)getCpuFrequencyMhz());

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BUTTON_LED, 0);
  ledcWrite(0, 0);
  Serial.printf("[button] pin %d (pull-up), LED pin %d\n", PIN_BUTTON, PIN_BUTTON_LED);

  synthBegin();
  displayBegin();
  computeLayout();
  drawAll();

  Serial.println(F("ready -- press the button"));
  Serial.println(F("=============================================="));
}

void loop() {
  static uint32_t lastReport = 0;

  if (displayHandleSerial()) { computeLayout(); drawAll(); }

  // Sing along: mouth opens wider for higher notes, so pitch is visible.
  const uint8_t midi = g_currentMidi;
  if (midi) {
    const float openness = constrain((midi - 55) / 36.0f, 0.15f, 1.0f);
    drawMouth(openness);
  } else {
    drawMouth(0.0f);
    if (millis() > nextBlink) {          // only blink when not singing
      nextBlink = millis() + random(2400, 5200);
      drawEyes(true);  delay(120);  drawEyes(false);
    }
  }
  drawNote(midi);

  pollButton();
  updateLed();
  synthTick();

  if (millis() - lastReport > 5000) {
    lastReport = millis();
    Serial.printf("[loop] heap %lu  mood %s  presses %lu\n",
                  (unsigned long)ESP.getFreeHeap(), MOOD_NAMES[mood],
                  (unsigned long)pressCount);
  }
  delay(8);
}
