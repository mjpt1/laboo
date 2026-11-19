#include "sound.h"
#include "es8311.h"
#include <math.h>

#define PA_PIN 15                     // speaker amplifier enable
#define RATE 16000                    // same rate as the microphone

static I2SClass* I2S = nullptr;
static QueueHandle_t q = nullptr;
static volatile uint32_t busyUntil = 0;
static es8311_handle_t codec = nullptr;
static int volume = 70;

// one "note": a tone that slides from f0 to f1 (0 Hz = rest)
enum Wave : uint8_t { SINE, SQUARE, NOISE };
struct Note { uint16_t f0, f1, ms; Wave w; uint8_t vib; uint8_t vol; };   // vib: vibrato depth in % (6 Hz)
#define END {0, 0, 0, SINE, 0, 0}

static const Note s_happy[]   = {{600, 1100, 80, SINE, 0, 90}, {0, 0, 30, SINE, 0, 0}, {800, 1500, 90, SINE, 0, 90}, END};
static const Note s_surpr[]   = {{380, 1400, 170, SINE, 0, 100}, END};
static const Note s_angry[]   = {{150, 115, 220, SQUARE, 30, 55}, {0, 0, 40, SINE, 0, 0}, {160, 100, 260, SQUARE, 30, 55}, END};
static const Note s_love[]    = {{520, 520, 140, SINE, 0, 80}, {0, 0, 40, SINE, 0, 0}, {780, 780, 280, SINE, 4, 80}, END};
static const Note s_scared[]  = {{1600, 900, 100, SINE, 0, 100}, {1500, 800, 100, SINE, 0, 100}, {1400, 600, 160, SINE, 0, 100}, END};
static const Note s_yawn[]    = {{700, 300, 900, SINE, 4, 70}, {350, 250, 450, SINE, 3, 50}, END};
static const Note s_morning[] = {{1047, 1047, 120, SINE, 0, 85}, {1319, 1319, 120, SINE, 0, 85}, {1568, 1568, 240, SINE, 2, 85}, END};
static const Note s_night[]   = {{784, 784, 200, SINE, 0, 60}, {659, 659, 200, SINE, 0, 60}, {523, 523, 420, SINE, 3, 60}, END};
static const Note s_whistle[] = {{1200, 1900, 260, SINE, 0, 80}, {0, 0, 90, SINE, 0, 0}, {1900, 1100, 320, SINE, 0, 80}, END};
static const Note s_dice[]    = {{1, 1, 14, NOISE, 0, 70}, {0, 0, 45, SINE, 0, 0}, {1, 1, 14, NOISE, 0, 70}, {0, 0, 60, SINE, 0, 0},
                                 {1, 1, 14, NOISE, 0, 70}, {0, 0, 80, SINE, 0, 0}, {1, 1, 14, NOISE, 0, 70}, {0, 0, 110, SINE, 0, 0},
                                 {1, 1, 14, NOISE, 0, 70}, {0, 0, 150, SINE, 0, 0}, {1, 1, 18, NOISE, 0, 70}, END};
static const Note s_go[]      = {{1000, 1000, 140, SQUARE, 0, 45}, END};
static const Note s_win[]     = {{880, 880, 100, SINE, 0, 90}, {1175, 1175, 100, SINE, 0, 90}, {1568, 1568, 260, SINE, 3, 90}, END};
static const Note s_lose[]    = {{420, 240, 420, SQUARE, 8, 45}, END};
static const Note s_pop[]     = {{900, 500, 45, SINE, 0, 80}, END};
static const Note* const SOUNDS[SND_COUNT] = {s_happy, s_surpr, s_angry, s_love, s_scared, s_yawn, s_morning,
                                              s_night, s_whistle, s_dice, s_go, s_win, s_lose, s_pop};

bool sound_codec_init() {
  pinMode(PA_PIN, OUTPUT);
  digitalWrite(PA_PIN, LOW);
  codec = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);
  if (!codec) return false;
  const es8311_clock_config_t clk = {false, false, true, RATE * 256, RATE};
  if (es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  es8311_voice_volume_set(codec, volume, NULL);
  es8311_microphone_config(codec, false);
  return true;
}
void sound_set_volume(int v) { volume = constrain(v, 0, 100); if (codec) es8311_voice_volume_set(codec, volume, NULL); }

static int16_t buf[256 * 2];
static void playNotes(const Note* n) {
  static uint32_t rng = 12345;
  float ph = 0;
  for (; n->ms; n++) {
    int total = RATE * n->ms / 1000, done = 0;
    const int att = RATE * 5 / 1000, rel = RATE * 15 / 1000;
    while (done < total) {
      int k = min(256, total - done);
      for (int i = 0; i < k; i++) {
        int s = done + i;
        float v = 0;
        if (n->f0) {
          float u = (float)s / total;
          float f = n->f0 + (n->f1 - n->f0) * u;
          if (n->vib) f *= 1.0f + n->vib / 100.0f * sinf(6.2832f * 6.0f * s / RATE);
          ph += f / RATE; if (ph >= 1) ph -= 1;
          if (n->w == SINE) v = sinf(6.2832f * ph);
          else if (n->w == SQUARE) v = ph < 0.5f ? 0.7f : -0.7f;
          else { rng = rng * 1664525u + 1013904223u; v = ((int32_t)rng >> 16) / 32768.0f; }
          float env = 1;
          if (s < att) env = (float)s / att;
          if (total - s < rel) env = fminf(env, (float)(total - s) / rel);
          v *= env * n->vol / 100.0f;
        }
        int16_t x = (int16_t)(v * 11000);
        buf[2 * i] = x; buf[2 * i + 1] = x;
      }
      I2S->write((uint8_t*)buf, k * 4);
      busyUntil = millis() + 300;
      done += k;
    }
  }
}

static void audioTask(void*) {
  memset(buf, 0, sizeof(buf));
  for (;;) {
    int id;
    if (xQueueReceive(q, &id, 0) == pdTRUE && id >= 0 && id < SND_COUNT) {
      digitalWrite(PA_PIN, HIGH);
      playNotes(SOUNDS[id]);
      memset(buf, 0, sizeof(buf));
    } else {
      I2S->write((uint8_t*)buf, sizeof(buf));     // silence keeps the DMA fed and paces this task
      if (millis() > busyUntil + 2000) digitalWrite(PA_PIN, LOW);   // amp off when quiet (no hiss)
    }
  }
}

void sound_start(I2SClass* i2s) {
  I2S = i2s;
  q = xQueueCreate(2, sizeof(int));
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 9, NULL, 0);
}
void sound_play(Sound s) { if (q) { int id = s; xQueueSend(q, &id, 0); busyUntil = millis() + 400; } }
bool sound_busy() { return millis() < busyUntil; }
