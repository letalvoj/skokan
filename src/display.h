#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_SPITFT.h>

// The panel has no MISO, so we cannot ask it anything -- driver, rotation and
// colour inversion are guesses. Keep them adjustable at runtime and save the
// winning combination to flash, so we calibrate by eye instead of by reflash.
enum DriverKind : uint8_t { DRV_ST7789 = 0, DRV_ILI9341 = 1 };

struct DisplayCfg {
  uint8_t driver;    // DriverKind
  uint8_t rotation;  // 0..3
  bool    invert;
};

extern DisplayCfg  dcfg;
extern Adafruit_GFX *gfx;   // whichever driver is live

// The same object, typed as the SPI driver rather than the drawing interface.
// Adafruit_GFX::drawRGBBitmap is NOT virtual, and Adafruit_SPITFT hides it
// with a bulk-SPI version that is ~50x faster. Anything blitting a whole
// framebuffer (the game) must go through `tft`, not `gfx`, or it will crawl.
extern Adafruit_SPITFT *tft;

void displayBegin();        // load config from flash, init the panel
void displayApply();        // re-init after a config change
void displaySave();         // persist current config

// SPI clock. 20 MHz is the safe breadboard default (see README learnings);
// a full-screen game is SPI-bound, so it offers 40 MHz as an opt-in.
void     displaySetSpeed(uint32_t hz);
uint32_t displaySpeed();
bool displayHandleSerial(); // returns true if the config changed
bool displayHandleChar(char c);  // same, for one already-read character
void displayCalibrationHelp();

int16_t scrW();
int16_t scrH();
void    pushRow(int16_t x, int16_t y, uint16_t *px, int16_t w);
