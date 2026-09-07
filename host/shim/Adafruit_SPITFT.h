// Host stand-in for the panel driver: an Adafruit_GFX whose pixels land in a
// plain RGB565 framebuffer instead of going out over SPI.
//
// It deliberately keeps the real class name and the real drawRGBBitmap
// signature, including the fact that it *hides* the non-virtual base version
// -- that shadowing is the whole reason display.h exposes `tft` separately,
// so the host build must reproduce it rather than paper over it.
#pragma once

#include <Adafruit_GFX.h>
#include <cstdint>

class Adafruit_SPITFT : public Adafruit_GFX {
public:
  Adafruit_SPITFT(int16_t w, int16_t h);
  ~Adafruit_SPITFT();

  void drawPixel(int16_t x, int16_t y, uint16_t color) override;
  void fillScreen(uint16_t color) override;

  using Adafruit_GFX::drawRGBBitmap;
  void drawRGBBitmap(int16_t x, int16_t y, uint16_t *pcolors, int16_t w, int16_t h);

  void setSPISpeed(uint32_t) {}
  // The harness always renders landscape; rotation is a hardware calibration
  // concern, not a visual one, so the framebuffer is simply fixed at 320x240.
  void setRotation(uint8_t) override {}

  const uint16_t *pixels() const { return fb_; }

private:
  uint16_t *fb_;
};
