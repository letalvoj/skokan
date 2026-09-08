// Just enough Arduino to compile the game core on macOS. See host/README.md.
//
// The clock is FAKE and driven by the harness (hostAdvance), so a run that
// would take five minutes on the board simulates in well under a second.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef ARDUINO
#define ARDUINO 200
#endif

typedef bool     boolean;
typedef uint8_t  byte;

#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif

// Arduino's flash-string trick: on a host there is only one address space.
class __FlashStringHelper;
#define PSTR(s) (s)
#define F(s)    (reinterpret_cast<const __FlashStringHelper *>(PSTR(s)))
#define PROGMEM
#define pgm_read_byte(a)    (*(const uint8_t *)(a))
#define pgm_read_word(a)    (*(const uint16_t *)(a))
#define pgm_read_dword(a)   (*(const uint32_t *)(a))
// Adafruit_GFX.cpp defines its own pgm_read_pointer, so leave that one alone.

inline float radians(float deg) { return deg * 0.017453292519943295f; }
inline float degrees(float rad) { return rad * 57.29577951308232f; }

// ---- time (fake) --------------------------------------------------------
extern uint32_t hostMillis;
inline uint32_t millis() { return hostMillis; }
inline uint32_t micros() { return hostMillis * 1000u; }
inline void     delay(uint32_t) {}

// ---- random -------------------------------------------------------------
inline long  random(long hi)          { return hi > 0 ? (long)(::rand() % hi) : 0; }
inline long  random(long lo, long hi) { return hi > lo ? lo + (long)(::rand() % (hi - lo)) : lo; }
inline void  randomSeed(unsigned s)   { ::srand(s); }
inline uint32_t esp_random()          { return (uint32_t)::rand(); }

// ---- GPIO / PWM (all no-ops except the button, which the harness drives) --
#define INPUT        0
#define OUTPUT       1
#define INPUT_PULLUP 2
#define LOW          0
#define HIGH         1

extern int hostButtonLevel;    // 0 = pressed (pull-up), 1 = released
extern int hostLedDuty;
extern bool hostQuiet;

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
int  digitalRead(int pin);
void ledcSetup(int ch, int freq, int bits);
void ledcAttachPin(int pin, int ch);
void ledcWrite(int ch, int duty);

// ---- String -------------------------------------------------------------
class String {
public:
  String() {}
  String(const char *s) : s_(s ? s : "") {}
  unsigned length() const { return (unsigned)s_.size(); }
  char operator[](unsigned i) const { return i < s_.size() ? s_[i] : 0; }
  const char *c_str() const { return s_.c_str(); }
private:
  std::string s_;
};

#include "Print.h"

// ---- Serial -------------------------------------------------------------
class HostSerial : public Print {
public:
  void   begin(unsigned long) {}
  int    available() { return 0; }
  int    read() { return -1; }
  size_t write(uint8_t c) override { if (!hostQuiet) fputc(c, stdout); return 1; }
};
extern HostSerial Serial;

// Arduino's macros. Defined last so the standard headers above stay intact.
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef abs
#define abs(x) ((x) > 0 ? (x) : -(x))
#endif
#ifndef constrain
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif

// FreeRTOS bits the game touches. There is one "core" on the host.
inline int xPortGetCoreID() { return 0; }
