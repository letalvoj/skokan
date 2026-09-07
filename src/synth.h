#pragma once
#include <Arduino.h>

// Chiptune voice for the MAX98357A. All the amp-timing and DMA-drain fixes
// found during step 1 live here, so every build gets them.

enum Wave : uint8_t { WAVE_SINE = 0, WAVE_TRIANGLE, WAVE_SQUARE };

void synthBegin();               // installs I2S and starts the task on core 1
void synthPlay(uint8_t jingle);  // 0..SYNTH_JINGLES-1, or 0xFF for random
bool synthBusy();
void synthTick();                // call from loop(): handles the idle mute
const char *synthJingleName(uint8_t i);

// ---- one-shot sound effects -------------------------------------------
// Notes are queued rather than played inline, so a game can fire a coin ping
// while a jump blip is still sounding instead of losing it. Unlike
// synthPlay(), these never refuse: they line up behind whatever is playing.
// A full queue drops the newest note (better than stuttering the oldest).
void synthBeep(uint8_t midi, uint16_t ms, Wave w = WAVE_SQUARE, uint8_t vol = 100);
void synthArp(const uint8_t *midis, uint8_t n, uint16_t msEach,
              Wave w = WAVE_SQUARE, uint8_t vol = 100);

// The amp takes ~250 ms to wake (see README). A game makes noise constantly,
// so hold it awake for the duration rather than paying that on every effect.
void synthHoldAmp(bool on);

extern const uint8_t SYNTH_JINGLES;

// What the synth is doing right now, so the face can sing along.
extern volatile uint8_t g_currentMidi;   // 0 when nothing is sounding
extern volatile uint8_t g_currentJingle;
