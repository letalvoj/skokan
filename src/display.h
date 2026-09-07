#pragma once
#include <Adafruit_GFX.h>

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

void displayBegin();        // load config from flash, init the panel
void displayApply();        // re-init after a config change
void displaySave();         // persist current config
bool displayHandleSerial(); // returns true if the config changed
bool displayHandleChar(char c);  // same, for one already-read character
void displayCalibrationHelp();

int16_t scrW();
int16_t scrH();
void    pushRow(int16_t x, int16_t y, uint16_t *px, int16_t w);
