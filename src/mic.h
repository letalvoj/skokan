#pragma once
#include <Arduino.h>

// I2S microphone. The driver is installed once, but the peripheral is stopped
// while we are not listening -- that leaves BCLK/WS genuinely idle rather than
// clocking away next to the display's SPI lines.

static const uint16_t MIC_FFT_N   = 512;
static const uint32_t MIC_RATE_HZ = 16000;
static const uint16_t MIC_BINS    = MIC_FFT_N / 2;   // 0..8 kHz

void micBegin();
void micStart();          // begin listening
void micStop();           // stop the clocks
bool micRunning();

// Fills bars[0..nbars-1] with 0..1 levels, log-spaced across frequency, and
// sets rms to overall loudness. Returns false if no block was ready.
bool micReadBars(float *bars, uint8_t nbars, float *rms);

// The bars are shown relative to a per-band noise floor the mic learns from
// the room, so office hum reads as "nothing" instead of half-scale. These
// nudge how far above that floor a band must be before it lights up, and how
// many dB of shouting it takes to fill the display.
void  micAdjustMargin(float dB);
void  micAdjustSpan(float dB);
float micMargin();
float micSpan();
