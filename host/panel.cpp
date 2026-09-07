// The virtual panel, the fake I/O the shims declare, and a BMP writer.
#include <Adafruit_SPITFT.h>
#include <Arduino.h>
#include <cstdio>

uint32_t hostMillis      = 0;
int      hostButtonLevel = HIGH;   // pull-up: HIGH = released
int      hostLedDuty     = 0;

int  digitalRead(int) { return hostButtonLevel; }
void ledcSetup(int, int, int) {}
void ledcAttachPin(int, int) {}
void ledcWrite(int, int duty) { hostLedDuty = duty; }

HostSerial Serial;

size_t Print::print(const String &s) { return print(s.c_str()); }

// ---------------------------------------------------------------------------
Adafruit_SPITFT::Adafruit_SPITFT(int16_t w, int16_t h) : Adafruit_GFX(w, h) {
  fb_ = new uint16_t[(size_t)w * h]();
}
Adafruit_SPITFT::~Adafruit_SPITFT() { delete[] fb_; }

void Adafruit_SPITFT::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= _width || y >= _height) return;
  fb_[(size_t)y * _width + x] = color;
}

void Adafruit_SPITFT::fillScreen(uint16_t color) {
  const size_t n = (size_t)_width * _height;
  for (size_t i = 0; i < n; i++) fb_[i] = color;
}

void Adafruit_SPITFT::drawRGBBitmap(int16_t x, int16_t y, uint16_t *pcolors,
                                    int16_t w, int16_t h) {
  for (int16_t r = 0; r < h; r++)
    for (int16_t c = 0; c < w; c++)
      drawPixel(x + c, y + r, pcolors[(size_t)r * w + c]);
}

// ---------------------------------------------------------------------------
// 24-bit BMP, scaled with nearest-neighbour so the 2x2 art blocks stay crisp.
// ---------------------------------------------------------------------------
static void put32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v, f); fputc(v >> 8, f); }

bool hostWriteBMP(const char *path, const uint16_t *fb,
                  int w, int h, int scale) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;

  const int ow = w * scale, oh = h * scale;
  const int rowBytes = ((ow * 3 + 3) / 4) * 4;
  const uint32_t dataSize = (uint32_t)rowBytes * oh;

  fputc('B', f); fputc('M', f);
  put32(f, 54 + dataSize); put32(f, 0); put32(f, 54);
  put32(f, 40); put32(f, ow); put32(f, oh);
  put16(f, 1); put16(f, 24);
  put32(f, 0); put32(f, dataSize);
  put32(f, 2835); put32(f, 2835); put32(f, 0); put32(f, 0);

  for (int oy = oh - 1; oy >= 0; oy--) {          // BMP rows run bottom-up
    const int sy = oy / scale;
    int written = 0;
    for (int ox = 0; ox < ow; ox++) {
      const uint16_t c = fb[(size_t)sy * w + (ox / scale)];
      const uint8_t r = (uint8_t)(((c >> 11) & 0x1F) << 3 | ((c >> 13) & 0x07));
      const uint8_t g = (uint8_t)(((c >> 5) & 0x3F) << 2 | ((c >> 9) & 0x03));
      const uint8_t b = (uint8_t)((c & 0x1F) << 3 | ((c >> 2) & 0x07));
      fputc(b, f); fputc(g, f); fputc(r, f);
      written += 3;
    }
    while (written++ < rowBytes) fputc(0, f);
  }
  fclose(f);
  return true;
}
