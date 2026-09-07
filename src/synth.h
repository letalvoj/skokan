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

extern const uint8_t SYNTH_JINGLES;

// What the synth is doing right now, so the face can sing along.
extern volatile uint8_t g_currentMidi;   // 0 when nothing is sounding
extern volatile uint8_t g_currentJingle;
