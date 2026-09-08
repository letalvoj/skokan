// synth.h on top of SDL audio, so the playable macOS build actually makes the
// noises the box does. The envelope and oscillator are copied from
// src/synth.cpp deliberately -- if the chiptune is going to be tuned by ear on
// a laptop, it has to be the same voice that comes out of the speaker.
//
// None of the ESP32 amp fixes (SD wake time, DMA drain) are here: they exist
// because of the MAX98357A, and a sound card has no such problem.
#include <SDL.h>          // sdl2-config --cflags points at the SDL2 dir
#include <math.h>
#include "../src/synth.h"

static const int RATE = 22050;

const uint8_t   SYNTH_JINGLES   = 3;
volatile uint8_t g_currentMidi   = 0;
volatile uint8_t g_currentJingle = 0;
static const char *NAMES[] = { "power-up", "ta-daa", "ode to joy" };
const char *synthJingleName(uint8_t i) { return NAMES[i % SYNTH_JINGLES]; }

struct QN { uint8_t midi; uint16_t ms; uint8_t wave; uint8_t vol; };
static const int QLEN = 64;
static QN  q[QLEN];
static int qHead = 0, qTail = 0;

static SDL_AudioDeviceID dev = 0;
static QN    cur       = { 0, 0, 0, 0 };
static int   notePos   = 0, noteTotal = 0;
static float phase     = 0.0f;

static inline float midiToHz(uint8_t m) { return 440.0f * powf(2.0f, (m - 69) / 12.0f); }

static inline float osc(float p, uint8_t w) {
  switch (w) {
    case WAVE_SINE:     return sinf(p * 2.0f * (float)M_PI);
    case WAVE_TRIANGLE: return 4.0f * fabsf(p - 0.5f) - 1.0f;
    default:            return p < 0.5f ? 1.0f : -1.0f;
  }
}

static void audioCB(void *, Uint8 *stream, int len) {
  int16_t *out = (int16_t *)stream;
  const int frames = len / (int)(2 * sizeof(int16_t));

  for (int i = 0; i < frames; i++) {
    if (notePos >= noteTotal) {                 // need the next note
      if (qTail != qHead) {
        cur       = q[qTail];
        qTail     = (qTail + 1) % QLEN;
        noteTotal = RATE * cur.ms / 1000;
        notePos   = 0;
        phase     = 0.0f;
        g_currentMidi = cur.midi;
      } else {
        g_currentMidi = 0;
        out[2 * i] = out[2 * i + 1] = 0;
        continue;
      }
    }

    float s = 0.0f;
    if (cur.midi) {
      s = osc(phase, cur.wave);
      phase += midiToHz(cur.midi) / RATE;
      if (phase >= 1.0f) phase -= 1.0f;

      const float t   = (float)notePos / (float)noteTotal;
      const float atk = fminf(1.0f, (float)notePos / (RATE * 0.008f));
      const float dec = 0.45f + 0.55f * expf(-4.0f * t);
      const float rel = t > 0.88f ? (1.0f - t) / 0.12f : 1.0f;
      s *= atk * dec * rel;
    }
    const int16_t v = (int16_t)(s * 50.0f * (float)cur.vol);
    out[2 * i] = out[2 * i + 1] = v;
    notePos++;
  }
}

static void push(uint8_t midi, uint16_t ms, Wave w, uint8_t vol) {
  if (!dev) return;
  SDL_LockAudioDevice(dev);
  const int next = (qHead + 1) % QLEN;
  if (next != qTail) { q[qHead] = { midi, ms, (uint8_t)w, vol }; qHead = next; }
  SDL_UnlockAudioDevice(dev);
}

void synthBegin() {
  SDL_AudioSpec want = {}, have = {};
  want.freq     = RATE;
  want.format   = AUDIO_S16SYS;
  want.channels = 2;
  want.samples  = 512;
  want.callback = audioCB;
  dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (!dev) { SDL_Log("no audio: %s", SDL_GetError()); return; }
  SDL_PauseAudioDevice(dev, 0);
}

void synthPlay(uint8_t) {}          // the game uses the effect API, not jingles
void synthTick() {}
void synthHoldAmp(bool) {}
bool synthBusy() { return qTail != qHead || notePos < noteTotal; }

void synthBeep(uint8_t midi, uint16_t ms, Wave w, uint8_t vol) { push(midi, ms, w, vol); }

void synthArp(const uint8_t *midis, uint8_t n, uint16_t msEach, Wave w, uint8_t vol) {
  for (uint8_t i = 0; i < n; i++) push(midis[i], msEach, w, vol);
}
