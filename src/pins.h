#pragma once
// All of these live on the 3V3 side of the dev board, so the whole build
// fits on one row of the breadboard.

// --- 2.8" TFT 240x320, GMT028-05 (ST7789 or ILI9341) -----------------
#define PIN_TFT_CS    10
#define PIN_TFT_SDA   11   // MOSI
#define PIN_TFT_SCK   12
#define PIN_TFT_DC    13
#define PIN_TFT_RST   14

// --- I2S microphone (INMP441-class) ----------------------------------
#define PIN_MIC_SCK    4   // bit clock
#define PIN_MIC_WS     5   // word select
#define PIN_MIC_SD     6   // data out of the mic

// --- MAX98357A I2S amplifier -----------------------------------------
#define PIN_AMP_BCLK  15
#define PIN_AMP_LRC   16
#define PIN_AMP_DIN   17

// --- Arcade button ----------------------------------------------------
#define PIN_BUTTON     8   // to GND, uses the internal pull-up
#define PIN_BUTTON_LED 9   // through 47R to the LED anode

// --- On dev board ------------------------------------------------------
#define PIN_RGB_LED   48   // WS2812 on most N16R8 boards

// Amp shutdown / mode select. LOW = amp off (silent), HIGH = on, left channel.
#define PIN_AMP_SD     7
