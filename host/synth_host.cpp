// Host implementation of synth.h. Silent: the harness cares about pixels.
#include "../src/synth.h"

const uint8_t SYNTH_JINGLES = 3;
volatile uint8_t g_currentMidi   = 0;
volatile uint8_t g_currentJingle = 0;

static const char *NAMES[] = { "power-up", "ta-daa", "ode to joy" };

void synthBegin() {}
void synthPlay(uint8_t) {}
bool synthBusy() { return false; }
void synthTick() {}
const char *synthJingleName(uint8_t i) { return NAMES[i % SYNTH_JINGLES]; }

void synthBeep(uint8_t, uint16_t, Wave, uint8_t) {}
void synthArp(const uint8_t *, uint8_t, uint16_t, Wave, uint8_t) {}
void synthHoldAmp(bool) {}
