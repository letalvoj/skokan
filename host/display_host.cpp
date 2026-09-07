// Host implementation of display.h -- same symbols, virtual panel behind them.
#include "../src/display.h"
#include <Arduino.h>

DisplayCfg      dcfg = { DRV_ILI9341, 1, false };
Adafruit_GFX   *gfx  = nullptr;
Adafruit_SPITFT *tft = nullptr;

static Adafruit_SPITFT panel(320, 240);   // landscape, as the game runs it
static uint32_t        spiHz = 20000000;

int16_t scrW() { return gfx ? gfx->width() : 320; }
int16_t scrH() { return gfx ? gfx->height() : 240; }

void pushRow(int16_t x, int16_t y, uint16_t *px, int16_t w) {
  tft->drawRGBBitmap(x, y, px, w, 1);
}

void displayApply() { gfx = &panel; tft = &panel; }
void displaySave()  {}
void displayCalibrationHelp() {}

void displayBegin() {
  displayApply();
  Serial.printf("[display] host panel %dx%d\n", gfx->width(), gfx->height());
}

void     displaySetSpeed(uint32_t hz) { spiHz = hz; }
uint32_t displaySpeed()               { return spiHz; }

bool displayHandleSerial() { return false; }
bool displayHandleChar(char) { return false; }
