// Minimal Arduino Print, enough for Adafruit_GFX (which subclasses it) and
// for the game's Serial/canvas print() and printf() calls.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

class __FlashStringHelper;
class String;

class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;

  virtual size_t write(const uint8_t *buf, size_t n) {
    size_t done = 0;
    while (n--) done += write(*buf++);
    return done;
  }

  size_t print(const char *s) { return s ? write((const uint8_t *)s, strlen(s)) : 0; }
  size_t print(char c)        { return write((uint8_t)c); }
  size_t print(const __FlashStringHelper *s) { return print((const char *)s); }
  size_t print(const String &s);
  size_t print(int v)           { return printf("%d", v); }
  size_t print(unsigned v)      { return printf("%u", v); }
  size_t print(long v)          { return printf("%ld", v); }
  size_t print(unsigned long v) { return printf("%lu", v); }
  size_t print(double v, int d = 2) { return printf("%.*f", d, v); }

  size_t println()             { return print("\r\n"); }
  template <typename T> size_t println(T v) { return print(v) + println(); }

  size_t printf(const char *fmt, ...) {
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    return write((const uint8_t *)buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
  }
};
