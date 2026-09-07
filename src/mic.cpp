#include <driver/i2s.h>
#include <arduinoFFT.h>
#include <math.h>
#include "mic.h"
#include "pins.h"

static const i2s_port_t IN_PORT = I2S_NUM_0;

static float   vReal[MIC_FFT_N];
static float   vImag[MIC_FFT_N];
// Read BOTH channels. Which slot the mic speaks in depends on its L/R pin,
// and a loose or floating L/R wire silently moves it to the other half of the
// frame. Reading stereo and picking the live side makes that a non-issue.
static int32_t rawBuf[MIC_FFT_N * 2];
static bool    g_running = false;
static int8_t  g_channel = -1;          // 0 = left slot, 1 = right slot

static const uint8_t MAX_BARS = 32;
static float   noiseFloor[MAX_BARS];
static bool    g_floorReady = false;
static float   g_marginDb   = 8.0f;     // how far above the room to start
static float   g_spanDb     = 28.0f;    // room -> full scale

static ArduinoFFT<float> FFT(vReal, vImag, MIC_FFT_N, (float)MIC_RATE_HZ);

bool micRunning() { return g_running; }

void micBegin() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate          = MIC_RATE_HZ;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;  // INMP441 is 24-in-32
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT; // both, we pick later
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 4;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = PIN_MIC_SCK;
  pins.ws_io_num    = PIN_MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = PIN_MIC_SD;

  ESP_ERROR_CHECK(i2s_driver_install(IN_PORT, &cfg, 0, NULL));
  ESP_ERROR_CHECK(i2s_set_pin(IN_PORT, &pins));
  i2s_stop(IN_PORT);            // idle until the button says otherwise
  g_running = false;

  Serial.printf("[mic] I2S%d  SCK=%d WS=%d SD=%d  %lu Hz, %u-pt FFT "
                "(%.1f Hz/bin, up to %lu Hz) -- stopped\n",
                (int)IN_PORT, PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD,
                (unsigned long)MIC_RATE_HZ, MIC_FFT_N,
                (float)MIC_RATE_HZ / MIC_FFT_N, (unsigned long)(MIC_RATE_HZ / 2));
}

void micStart() {
  if (g_running) return;
  i2s_start(IN_PORT);
  i2s_zero_dma_buffer(IN_PORT);
  g_running = true;
  g_channel = -1;              // re-detect the live slot each time we start
  g_floorReady = false;        // and re-learn the room
  Serial.println(F("[mic] listening"));
}

void micStop() {
  if (!g_running) return;
  i2s_stop(IN_PORT);
  g_running = false;
  Serial.println(F("[mic] stopped"));
}

// Screen bar -> frequency, log spaced. 80 Hz to 6 kHz covers voice and
// whistling without wasting half the display on inaudible hiss.
static const float F_LO = 80.0f, F_HI = 6000.0f;

bool micReadBars(float *bars, uint8_t nbars, float *rms) {
  if (!g_running) return false;
  if (nbars > MAX_BARS) return false;

  size_t bytesRead = 0;
  const esp_err_t err = i2s_read(IN_PORT, rawBuf, sizeof(rawBuf), &bytesRead,
                                 pdMS_TO_TICKS(40));
  if (err != ESP_OK || bytesRead < sizeof(rawBuf) / 2) return false;

  const uint16_t frames = min<uint16_t>(MIC_FFT_N, (bytesRead / sizeof(int32_t)) / 2);

  // Decide which slot the mic is actually using, by energy. A dead slot reads
  // as exact zeros, so this is unambiguous, and we only need to decide once.
  if (g_channel < 0) {
    double eL = 0.0, eR = 0.0;
    for (uint16_t i = 0; i < frames; i++) {
      eL += fabs((double)(rawBuf[i * 2]     >> 8));
      eR += fabs((double)(rawBuf[i * 2 + 1] >> 8));
    }
    if (eL + eR > 0.0) {
      g_channel = (eR > eL) ? 1 : 0;
      Serial.printf("[mic] data is in the %s slot (L=%.0f R=%.0f) -- "
                    "if that says right, the L/R pin is not grounded\n",
                    g_channel ? "RIGHT" : "LEFT", eL, eR);
    }
  }
  const uint8_t ch = (g_channel < 0) ? 0 : (uint8_t)g_channel;

  double sumSq = 0.0;
  for (uint16_t i = 0; i < MIC_FFT_N; i++) {
    // Drop the 8 empty low bits of the 24-in-32 sample, scale to about -1..1.
    const float s = (i < frames)
                  ? (float)(rawBuf[i * 2 + ch] >> 8) / 8388608.0f : 0.0f;
    vReal[i] = s;
    vImag[i] = 0.0f;
    sumSq += (double)s * s;
  }
  *rms = constrain(sqrtf((float)(sumSq / MIC_FFT_N)) * 12.0f, 0.0f, 1.0f);

  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  for (uint8_t b = 0; b < nbars; b++) {
    const float t0 = (float)b / nbars, t1 = (float)(b + 1) / nbars;
    const uint16_t lo = (uint16_t)(F_LO * powf(F_HI / F_LO, t0) * MIC_FFT_N / MIC_RATE_HZ);
    const uint16_t hi = max<uint16_t>(lo + 1,
                        (uint16_t)(F_LO * powf(F_HI / F_LO, t1) * MIC_FFT_N / MIC_RATE_HZ));
    float peak = 0.0f;
    for (uint16_t i = lo; i < min<uint16_t>(hi, MIC_BINS); i++)
      if (vReal[i] > peak) peak = vReal[i];
    // Ears are logarithmic, so work in decibels, not raw magnitude.
    const float db = 20.0f * log10f(peak + 1e-6f);

    // Track a per-band noise floor: fall fast, rise slowly. That makes it
    // settle onto the quietest recent level in each band -- the AC hum, the
    // fan, the room -- without being dragged up by speech. Every band gets
    // its own floor, so a loud 50 Hz hum does not desensitise the whole
    // display the way a single global threshold would.
    if (!g_floorReady) noiseFloor[b] = db;
    else               noiseFloor[b] += (db > noiseFloor[b]) ? 0.03f : -0.8f;

    bars[b] = constrain((db - noiseFloor[b] - g_marginDb) / g_spanDb, 0.0f, 1.0f);
  }
  g_floorReady = true;
  return true;
}

void micAdjustMargin(float dB) {
  g_marginDb = constrain(g_marginDb + dB, 0.0f, 40.0f);
  Serial.printf("[mic] margin above noise floor -> %.0f dB\n", g_marginDb);
}

void micAdjustSpan(float dB) {
  g_spanDb = constrain(g_spanDb + dB, 6.0f, 60.0f);
  Serial.printf("[mic] span (quiet to full scale) -> %.0f dB\n", g_spanDb);
}

float micMargin() { return g_marginDb; }
float micSpan()   { return g_spanDb; }
