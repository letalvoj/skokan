#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_ILI9341.h>
#include "display.h"
#include "pins.h"

static const uint32_t SPI_HZ = 20000000;   // 40 MHz is too much for breadboard jumpers

static SPIClass        tftSPI(FSPI);
static Adafruit_ST7789  st7789(&tftSPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
static Adafruit_ILI9341 ili9341(&tftSPI, PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_RST);
static Preferences      prefs;

DisplayCfg   dcfg = { DRV_ILI9341, 0, false };  // confirmed by eye on this panel
Adafruit_GFX *gfx = nullptr;

int16_t scrW() { return gfx ? gfx->width()  : 240; }
int16_t scrH() { return gfx ? gfx->height() : 320; }

void pushRow(int16_t x, int16_t y, uint16_t *px, int16_t w) {
  gfx->drawRGBBitmap(x, y, px, w, 1);
}

static const char *driverName() {
  return dcfg.driver == DRV_ST7789 ? "ST7789" : "ILI9341";
}

void displayApply() {
  Serial.printf("[display] driver=%s rotation=%u invert=%s\n",
                driverName(), dcfg.rotation, dcfg.invert ? "yes" : "no");

  if (dcfg.driver == DRV_ST7789) {
    st7789.init(240, 320);
    st7789.setSPISpeed(SPI_HZ);
    st7789.setRotation(dcfg.rotation);
    // ST7789 panels normally want inversion ON; the library already does that
    // in init(), so our flag toggles it back off when the panel disagrees.
    st7789.invertDisplay(!dcfg.invert);
    gfx = &st7789;
  } else {
    ili9341.begin(SPI_HZ);
    ili9341.setRotation(dcfg.rotation);
    ili9341.invertDisplay(dcfg.invert);
    gfx = &ili9341;
  }
  Serial.printf("[display] panel reports %dx%d\n", gfx->width(), gfx->height());
}

void displaySave() {
  prefs.begin("mluvitko", false);
  prefs.putUChar("drv", dcfg.driver);
  prefs.putUChar("rot", dcfg.rotation);
  prefs.putBool("inv", dcfg.invert);
  prefs.end();
  Serial.println(F("[display] config saved to flash"));
}

void displayCalibrationHelp() {
  Serial.println(F("---- display calibration (type in the serial monitor) ----"));
  Serial.println(F("  d = switch driver (ST7789 <-> ILI9341)"));
  Serial.println(F("  r = next rotation (0,1,2,3)"));
  Serial.println(F("  i = toggle colour inversion"));
  Serial.println(F("  s = save this combination to flash"));
  Serial.println(F("  ? = show current settings"));
  Serial.println(F("----------------------------------------------------------"));
}

void displayBegin() {
  tftSPI.begin(PIN_TFT_SCK, -1 /* no MISO on this module */, PIN_TFT_SDA, PIN_TFT_CS);

  prefs.begin("mluvitko", false);  // read-write, so the namespace exists on first boot
  dcfg.driver   = prefs.getUChar("drv", DRV_ILI9341);
  dcfg.rotation = prefs.getUChar("rot", 0);
  dcfg.invert   = prefs.getBool("inv", false);
  prefs.end();

  displayApply();
  displayCalibrationHelp();
}

bool displayHandleSerial() {
  if (!Serial.available()) return false;
  return displayHandleChar((char)Serial.read());
}

bool displayHandleChar(char c) {

  switch (c) {
    case 'd': dcfg.driver = (dcfg.driver == DRV_ST7789) ? DRV_ILI9341 : DRV_ST7789; break;
    case 'r': dcfg.rotation = (dcfg.rotation + 1) & 3; break;
    case 'i': dcfg.invert = !dcfg.invert; break;
    case 's': displaySave(); return false;
    case '?': Serial.printf("[display] driver=%s rotation=%u invert=%s\n",
                            driverName(), dcfg.rotation, dcfg.invert ? "yes" : "no");
              return false;
    default:  return false;
  }
  displayApply();
  return true;
}
