#include <driver/i2s.h>
#include <math.h>
#include "synth.h"
#include "pins.h"

static const uint32_t   OUT_RATE_HZ = 22050;
static const i2s_port_t OUT_PORT    = I2S_NUM_1;

// The amp needs a few hundred ms after SD goes high before it can be fed real
// audio: it samples SD to pick its channel mode, then runs click-and-pop
// suppression. Measured on this board: 60 ms hisses, 250 ms is clean.
static const uint16_t AMP_WAKE_MS     = 250;
static const uint32_t AMP_IDLE_OFF_MS = 4000;

// The I2S DMA holds 8 * 256 = 2048 frames, ~93 ms at 22050 Hz. When we stop
// writing, the driver does NOT go quiet -- it re-sends the last buffer
// forever. Every sound must be followed by more than a full buffer depth of
// silence or the final note loops.
static const uint16_t DMA_DRAIN_MS = 200;

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
static const Note *JINGLES[]      = { JINGLE_POWERUP, JINGLE_TADAA, JINGLE_JOY };
static const char *JINGLE_NAMES[] = { "power-up", "ta-daa", "ode to joy" };
const uint8_t SYNTH_JINGLES = 3;

static const char *NOTE_NAMES[12] =
  {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

static volatile bool     g_playing      = false;
static volatile bool     g_ampOn        = false;
static volatile bool     g_ampHold      = false;
static volatile uint32_t g_ampIdleSince = 0;

// Everything the synth task plays comes off this queue, jingles included.
// It exists so short effects can chain (coin ping behind a jump blip) instead
// of being dropped the way the old "one jingle at a time" request slot did.
struct QNote { uint8_t midi; uint16_t ms; uint8_t wave; uint8_t vol; };
static const uint8_t QLEN = 48;
static QNote           g_q[QLEN];
static volatile uint8_t g_qHead = 0, g_qTail = 0;
static portMUX_TYPE     g_qMux  = portMUX_INITIALIZER_UNLOCKED;

static bool qPush(uint8_t midi, uint16_t ms, Wave w, uint8_t vol) {
  bool ok = false;
  portENTER_CRITICAL(&g_qMux);
  const uint8_t next = (uint8_t)((g_qHead + 1) % QLEN);
  if (next != g_qTail) {
    g_q[g_qHead] = { midi, ms, (uint8_t)w, vol };
    g_qHead   = next;
    g_playing = true;   // set here, not in the task, so synthBusy() is
    ok        = true;   // truthful the instant the caller enqueues
  }
  portEXIT_CRITICAL(&g_qMux);
  return ok;
}

static bool qPop(QNote *out) {
  bool ok = false;
  portENTER_CRITICAL(&g_qMux);
  if (g_qTail != g_qHead) {
    *out   = g_q[g_qTail];
    g_qTail = (uint8_t)((g_qTail + 1) % QLEN);
    ok     = true;
  }
  portEXIT_CRITICAL(&g_qMux);
  return ok;
}

volatile uint8_t g_currentMidi   = 0;
volatile uint8_t g_currentJingle = 0;

static Wave g_wave = WAVE_SINE;

const char *synthJingleName(uint8_t i) { return JINGLE_NAMES[i % SYNTH_JINGLES]; }
bool synthBusy() { return g_playing; }

static inline float midiToHz(uint8_t m) {
  return 440.0f * powf(2.0f, (m - 69) / 12.0f);
}

static inline float oscillator(float phase, Wave wave) {
  switch (wave) {
    case WAVE_SINE:     return sinf(phase * 2.0f * PI);
    case WAVE_TRIANGLE: return 4.0f * fabsf(phase - 0.5f) - 1.0f;
    default:            return phase < 0.5f ? 1.0f : -1.0f;
  }
}

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

static void ampEnable(bool on) {
  digitalWrite(PIN_AMP_SD, on ? HIGH : LOW);
  g_ampOn = on;
}

// Discard anything stale while still muted, so it cannot be heard, then wake.
static void ampWakeForPlayback() {
  if (g_ampOn) return;
  i2s_zero_dma_buffer(OUT_PORT);
  ampEnable(true);
  flushSilence(AMP_WAKE_MS);
}

static void playNote(uint8_t midi, uint16_t ms, Wave wave, uint8_t vol) {
  const uint32_t total = (uint32_t)((OUT_RATE_HZ * (uint32_t)ms) / 1000);
  const float    step  = midi ? midiToHz(midi) / (float)OUT_RATE_HZ : 0.0f;

  static int16_t buf[256 * 2];
  float    phase = 0.0f;
  uint32_t done  = 0;

  g_currentMidi = midi;

  while (done < total) {
    const uint32_t n = min<uint32_t>(256, total - done);
    for (uint32_t i = 0; i < n; i++) {
      float s = 0.0f;
      if (midi) {
        s = oscillator(phase, wave);
        phase += step;
        if (phase >= 1.0f) phase -= 1.0f;

        const float t   = (float)(done + i) / (float)total;
        const float atk = min(1.0f, (float)(done + i) / (OUT_RATE_HZ * 0.008f));
        const float dec = 0.45f + 0.55f * expf(-4.0f * t);
        const float rel = t > 0.88f ? (1.0f - t) / 0.12f : 1.0f;
        s *= atk * dec * rel;
      }
      const int16_t v = (int16_t)(s * 50.0f * (float)vol);
      buf[i * 2] = v;
      buf[i * 2 + 1] = v;
    }
    size_t wrote = 0;
    i2s_write(OUT_PORT, buf, n * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
    done += n;
  }
}

static void synthTask(void *) {
  QNote n;
  for (;;) {
    if (!qPop(&n)) {
      // Queue just ran dry: this is the only moment the DMA must be drained,
      // whether that was the end of a jingle or of a single game blip.
      if (g_playing) {
        g_currentMidi = 0;
        flushSilence(DMA_DRAIN_MS);    // push the last note out, or it loops
        g_ampIdleSince = millis();
        g_playing      = false;
      }
      vTaskDelay(pdMS_TO_TICKS(4));
      continue;
    }
    ampWakeForPlayback();              // no-op once the amp is already up
    playNote(n.midi, n.ms, (Wave)n.wave, n.vol);
  }
}

void synthBegin() {
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

  Serial.printf("[synth] I2S%d  BCLK=%d LRC=%d DIN=%d  %lu Hz, amp SD=%d\n",
                (int)OUT_PORT, PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DIN,
                (unsigned long)OUT_RATE_HZ, PIN_AMP_SD);

  xTaskCreatePinnedToCore(synthTask, "synth", 4096, NULL, 2, NULL, 1);
}

void synthPlay(uint8_t which) {
  if (g_playing) return;             // one jingle at a time, as before
  const uint8_t idx = (which == 0xFF) ? (uint8_t)random(SYNTH_JINGLES)
                                      : (uint8_t)(which % SYNTH_JINGLES);
  g_currentJingle = idx;

  Serial.printf("[synth] \"%s\":", JINGLE_NAMES[idx]);
  for (const Note *n = JINGLES[idx]; n->ms; n++) {
    if (n->midi) Serial.printf(" %s%d", NOTE_NAMES[n->midi % 12], n->midi / 12 - 1);
    qPush(n->midi, n->ms, g_wave, 100);
  }
  Serial.println();
}

void synthBeep(uint8_t midi, uint16_t ms, Wave w, uint8_t vol) {
  qPush(midi, ms, w, vol);
}

void synthArp(const uint8_t *midis, uint8_t n, uint16_t msEach, Wave w, uint8_t vol) {
  for (uint8_t i = 0; i < n; i++) qPush(midis[i], msEach, w, vol);
}

void synthHoldAmp(bool on) {
  g_ampHold = on;
  if (on) g_ampIdleSince = millis();
}

void synthTick() {
  if (g_ampHold) { g_ampIdleSince = millis(); return; }
  if (g_ampOn && !g_playing && g_ampIdleSince &&
      millis() - g_ampIdleSince > AMP_IDLE_OFF_MS) {
    ampEnable(false);
    i2s_zero_dma_buffer(OUT_PORT);
    g_ampIdleSince = 0;
    Serial.println(F("[synth] amp idle -> muted"));
  }
}
