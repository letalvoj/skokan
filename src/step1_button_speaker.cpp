// esp32-mluvitko -- STEP 1: button + speaker only.
//
// No display, no microphone, no SPI, no I2S input. If the audio is bad here,
// it is the amp, the speaker or the power rail -- nothing else is running.
//
// Serial keys:  1/2/3 pick a jingle   w cycle waveform
//               +/-   volume          space replay last

#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>
#include "pins.h"

static const uint32_t   OUT_RATE_HZ = 22050;
static const i2s_port_t OUT_PORT    = I2S_NUM_1;

struct Note { uint8_t midi; uint16_t ms; };

static const Note JINGLE_POWERUP[] = {
  {72, 90}, {76, 90}, {79, 90}, {84, 90}, {88, 320}, {0, 0}
};
static const Note JINGLE_TADAA[] = {
  {67, 160}, {0, 40}, {72, 160}, {0, 40}, {76, 420}, {0, 0}
};
static const Note JINGLE_JOY[] = {
  {64, 220}, {64, 220}, {65, 220}, {67, 220},
  {67, 220}, {65, 220}, {64, 220}, {62, 380}, {0, 0}
};
static const Note *JINGLES[]     = { JINGLE_POWERUP, JINGLE_TADAA, JINGLE_JOY };
static const char *JINGLE_NAMES[] = { "power-up", "ta-daa", "ode to joy" };

enum Wave : uint8_t { WAVE_SINE = 0, WAVE_TRIANGLE, WAVE_SQUARE };
static const char *WAVE_NAMES[] = { "sine", "triangle", "square" };

// Start gentle: a sine at modest volume is the kindest thing you can ask of a
// small amp on a sagging 3.3V rail. Work up from here.
static volatile Wave    g_wave   = WAVE_SINE;
static volatile int16_t g_volume = 5000;      // peak amplitude, max 32767
static volatile int8_t  g_request = -1;
static volatile bool    g_playing = false;
// Diagnostic: keep the amp permanently enabled instead of muting between
// jingles. If the "chrrr" vanishes with this on, the noise is the amp's
// turn-on transient. If it stays, it is the power rail sagging under inrush.
static volatile bool    g_ampAlwaysOn = false;
// The LED runs 5 kHz PWM -- squarely inside the audio band, on the same rail.
static volatile bool    g_ledEnabled  = true;
static uint8_t          g_lastJingle = 0;

static const char *NOTE_NAMES[12] =
  {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

static inline float midiToHz(uint8_t m) {
  return 440.0f * powf(2.0f, (m - 69) / 12.0f);
}

static inline float oscillator(float phase) {
  switch (g_wave) {
    case WAVE_SINE:     return sinf(phase * 2.0f * PI);
    case WAVE_TRIANGLE: return 4.0f * fabsf(phase - 0.5f) - 1.0f;
    default:            return phase < 0.5f ? 1.0f : -1.0f;
  }
}

// The amp needs a few hundred ms after SD goes high before it can be fed real
// audio: it samples SD to pick its channel mode, then runs its click-and-pop
// suppression. Measured on this board: 60 ms is far too short (audible hiss),
// 200 ms is clean. So we wake it once, and then leave it awake for a few
// seconds -- repeated button presses cost no extra delay, and it still mutes
// itself when idle so there is no click when the rail wobbles.
static const uint16_t AMP_WAKE_MS = 250;
static const uint32_t AMP_IDLE_OFF_MS = 4000;

// The I2S DMA holds dma_buf_count * dma_buf_len frames = 8 * 256 = 2048,
// which at 22050 Hz is ~93 ms. When we stop writing, the driver does NOT go
// quiet -- it re-sends the last buffer forever. So every time we finish a
// sound we must push at least a full buffer depth of silence through, or the
// last note loops. Round up generously.
static const uint16_t DMA_DRAIN_MS = 200;

static volatile bool     g_ampOn = false;
static volatile uint32_t g_ampIdleSince = 0;

static void flushSilence(uint16_t ms);   // defined below

static void ampEnable(bool on) {
  digitalWrite(PIN_AMP_SD, on ? HIGH : LOW);
  g_ampOn = on;
}

// Wake the amp for playback. Anything stale left in the DMA buffers is
// discarded while we are still muted, so it cannot be heard.
static void ampWakeForPlayback() {
  if (g_ampOn) return;              // already awake, buffers already drained
  i2s_zero_dma_buffer(OUT_PORT);
  ampEnable(true);
  flushSilence(AMP_WAKE_MS);
}

static void playNote(uint8_t midi, uint16_t ms) {
  const uint32_t total = (uint32_t)((OUT_RATE_HZ * (uint32_t)ms) / 1000);
  const float    step  = midi ? midiToHz(midi) / (float)OUT_RATE_HZ : 0.0f;

  static int16_t buf[256 * 2];
  float    phase = 0.0f;
  uint32_t done  = 0;

  while (done < total) {
    const uint32_t n = min<uint32_t>(256, total - done);
    for (uint32_t i = 0; i < n; i++) {
      float s = 0.0f;
      if (midi) {
        s = oscillator(phase);
        phase += step;
        if (phase >= 1.0f) phase -= 1.0f;

        const float t   = (float)(done + i) / (float)total;
        const float atk = min(1.0f, (float)(done + i) / (OUT_RATE_HZ * 0.008f));
        const float dec = 0.45f + 0.55f * expf(-4.0f * t);
        const float rel = t > 0.88f ? (1.0f - t) / 0.12f : 1.0f;
        s *= atk * dec * rel;
      }
      const int16_t v = (int16_t)(s * g_volume);
      buf[i * 2] = v;
      buf[i * 2 + 1] = v;
    }
    size_t wrote = 0;
    i2s_write(OUT_PORT, buf, n * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
    done += n;
  }
}

// Push real silence through the I2S stream for a while. Merely waiting is not
// the same thing: the DMA keeps re-sending whatever was last in its buffers,
// and the amp wants a settled, actively-clocked signal before the first note.
static void flushSilence(uint16_t ms) {
  static int16_t quiet[256 * 2] = { 0 };
  const uint32_t total = (uint32_t)((OUT_RATE_HZ * (uint32_t)ms) / 1000);
  uint32_t done = 0;
  while (done < total) {
    const uint32_t n = min<uint32_t>(256, total - done);
    size_t wrote = 0;
    i2s_write(OUT_PORT, quiet, n * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
    done += n;
  }
}

// A steady tone at a fixed amplitude -- no envelope, no note changes. Any
// noise you hear under this is not the synth.
static void playSteady(float hz, uint16_t ms) {
  static int16_t buf[256 * 2];
  const uint32_t total = (uint32_t)((OUT_RATE_HZ * (uint32_t)ms) / 1000);
  const float    step  = hz / (float)OUT_RATE_HZ;
  float    phase = 0.0f;
  uint32_t done  = 0;
  while (done < total) {
    const uint32_t n = min<uint32_t>(256, total - done);
    for (uint32_t i = 0; i < n; i++) {
      const int16_t v = (int16_t)(sinf(phase * 2.0f * PI) * g_volume);
      phase += step; if (phase >= 1.0f) phase -= 1.0f;
      buf[i * 2] = v; buf[i * 2 + 1] = v;
    }
    size_t wrote = 0;
    i2s_write(OUT_PORT, buf, n * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
    done += n;
  }
}

static const int8_t REQ_SILENCE = 10;   // amp on, perfect digital silence
static const int8_t REQ_TONE    = 11;   // amp on, steady 440 Hz

static void synthTask(void *) {
  for (;;) {
    if (g_request < 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

    if (g_request == REQ_SILENCE || g_request == REQ_TONE) {
      const bool tone = (g_request == REQ_TONE);
      g_request = -1;
      g_playing = true;
      Serial.printf("[test] amp ON, 3 s of %s -- listen for hiss\n",
                    tone ? "steady 440 Hz" : "pure digital silence");
      ampWakeForPlayback();
      if (tone) { playSteady(440.0f, 3000); }
      else      { flushSilence(3000); }
      flushSilence(DMA_DRAIN_MS);
      g_ampIdleSince = millis();
      Serial.println(F("[test] done"));
      g_playing = false;
      continue;
    }

    const uint8_t idx = (uint8_t)g_request;
    g_request = -1;
    g_playing = true;

    Serial.printf("[synth] \"%s\"  %s @ vol %d :",
                  JINGLE_NAMES[idx], WAVE_NAMES[g_wave], g_volume);
    ampWakeForPlayback();

    for (const Note *n = JINGLES[idx]; n->ms; n++) {
      if (n->midi) Serial.printf(" %s%d", NOTE_NAMES[n->midi % 12], n->midi / 12 - 1);
      playNote(n->midi, n->ms);
    }
    Serial.println();

    flushSilence(DMA_DRAIN_MS);   // push the last note out of the DMA buffers
    g_ampIdleSince = millis();
    g_playing = false;
  }
}

static void play(uint8_t idx) {
  if (g_playing) return;
  g_lastJingle = idx % 3;
  g_request = (int8_t)g_lastJingle;
}

// ---- button ------------------------------------------------------------
static bool     ledOn = false, lastStable = true, lastReading = true;
static uint32_t pressCount = 0, lastChange = 0;

static void pollButton() {
  const bool reading = digitalRead(PIN_BUTTON);
  if (reading != lastReading) { lastReading = reading; lastChange = millis(); }
  if (millis() - lastChange > 25 && reading != lastStable) {
    lastStable = reading;
    if (!reading) {
      pressCount++;
      ledOn = !ledOn;
      Serial.printf("[button] press #%lu -> LED %s\n",
                    (unsigned long)pressCount, ledOn ? "ON" : "off");
      play(random(3));
    }
  }
}

static void handleSerial() {
  if (!Serial.available()) return;
  switch ((char)Serial.read()) {
    case '1': play(0); break;
    case '2': play(1); break;
    case '3': play(2); break;
    case ' ': play(g_lastJingle); break;
    case 'w': g_wave = (Wave)((g_wave + 1) % 3);
              Serial.printf("[synth] waveform -> %s\n", WAVE_NAMES[g_wave]); break;
    case '+': g_volume = min(30000, g_volume + 2500);
              Serial.printf("[synth] volume -> %d\n", g_volume); break;
    case '-': g_volume = max(1000, g_volume - 2500);
              Serial.printf("[synth] volume -> %d\n", g_volume); break;
    case 'z': g_request = REQ_SILENCE; break;
    case 't': g_request = REQ_TONE; break;
    case 'l': g_ledEnabled = !g_ledEnabled;
              Serial.printf("[button] LED PWM -> %s\n",
                            g_ledEnabled ? "enabled" : "DISABLED (ruling out PWM noise)");
              break;
    case 'a': g_ampAlwaysOn = !g_ampAlwaysOn;
              ampEnable(g_ampAlwaysOn);
              Serial.printf("[synth] amp always-on -> %s\n",
                            g_ampAlwaysOn ? "YES (never mutes)" : "no (mutes when idle)");
              break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);
  delay(150);

  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F("  esp32-mluvitko -- STEP 1: button + speaker"));
  Serial.println(F("  (display and microphone are NOT running)"));
  Serial.println(F("=============================================="));

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BUTTON_LED, 0);
  ledcWrite(0, 0);

  pinMode(PIN_AMP_SD, OUTPUT);
  ampEnable(false);

  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = OUT_RATE_HZ;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = PIN_AMP_BCLK;
  pins.ws_io_num    = PIN_AMP_LRC;
  pins.data_out_num = PIN_AMP_DIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  ESP_ERROR_CHECK(i2s_driver_install(OUT_PORT, &cfg, 0, NULL));
  ESP_ERROR_CHECK(i2s_set_pin(OUT_PORT, &pins));
  i2s_zero_dma_buffer(OUT_PORT);

  Serial.printf("[synth] I2S%d  BCLK=%d LRC=%d DIN=%d  %lu Hz\n",
                (int)OUT_PORT, PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DIN,
                (unsigned long)OUT_RATE_HZ);
  Serial.printf("[synth] waveform %s, volume %d\n", WAVE_NAMES[g_wave], g_volume);
  Serial.printf("[button] pin %d, LED pin %d\n", PIN_BUTTON, PIN_BUTTON_LED);
  Serial.println(F("keys: 1/2/3 jingle  space replay  w waveform  +/- volume"));
  Serial.println(F("      z silence-test  t 440Hz-tone  a amp-always-on  l LED-PWM off"));
  Serial.println(F("=============================================="));

  xTaskCreatePinnedToCore(synthTask, "synth", 4096, NULL, 2, NULL, 1);
}

void loop() {
  handleSerial();
  pollButton();

  // Put the amp back to sleep once it has been idle a while, so it is not
  // sitting there amplifying rail noise all day.
  if (g_ampOn && !g_ampAlwaysOn && !g_playing &&
      g_ampIdleSince && millis() - g_ampIdleSince > AMP_IDLE_OFF_MS) {
    i2s_zero_dma_buffer(OUT_PORT);
    ampEnable(false);
    g_ampIdleSince = 0;
    Serial.println(F("[synth] amp idle -> muted"));
  }

  uint8_t duty = 0;
  if (ledOn && g_ledEnabled) {
    const float t = (millis() % 2400) / 2400.0f;
    duty = (uint8_t)(90 + 165 * (0.5f - 0.5f * cosf(t * 2.0f * PI)));
  }
  ledcWrite(0, duty);

  delay(5);
}
