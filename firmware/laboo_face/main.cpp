/*
  LABOO - desk robot (standalone, v3.1)
  Board : Waveshare ESP32-S3-Touch-LCD-1.85C
  No laptop needed: Lumo-style face that reacts to touch and sound, has little
  sounds of its own, dances to music, yawns at night, plays two tiny games and
  shows a Persian clock when nobody plays with it.

  Touch
    tap ............... happy "chirp"
    4 quick taps ...... angry (red eyes, shivers, grumbles)
    stroke / pet ...... love (heart eyes)
    long press ........ show the clock right away
    swipe left/right .. dice  (tap to roll again)
    swipe up/down ..... reaction game: wait for green eyes, then tap fast
    on the clock ...... any touch brings the face back
  Sound
    sudden loud sound (clap) ... surprised
    long loud noise ............ scared
    someone talking ............ eyes open a bit and look at you
    music with a beat .......... dances (hops and sways on the beat)
  Time of day
    06-11 first hello of the day ... "صبح بخیر" + morning tune
    22-05 first touch of the night . yawn + "شب بخیر"; yawns now and then; clock dimmed 23-07
  Idle
    IDLE_TO_CLOCK_MS without touch or sound -> clock. During the day it sometimes gets
    bored on the clock: peeks out, looks around and whistles.

  USB (optional, 921600 baud, text lines):
    TIME <y> <mo> <d> <h> <mi> <s>   set the clock (Tehran time), kept by the RTC chip
    NOW                              -> current time on the board
    E <emotion> [ms]                 neutral happy sad angry surprised love confused sleepy scared
    SND <0..13>  VOL <0..100>        play a sound / speaker volume
    DICE | REACT | YAWN | BORED      try the extras
    CLOCK | FACE                     switch screen
    B <0..100>                       backlight
    PING                             -> "hello laboo-desk"

  Arduino IDE settings (Tools menu):
    Board ESP32S3 Dev Module, USB CDC On Boot Enabled, USB Mode Hardware CDC and JTAG,
    Flash 16MB, PSRAM OPI PSRAM, Partition 16M Flash (3MB APP/9.9MB FATFS)
*/
#include <Arduino.h>
#include <Wire.h>
#include "ESP_I2S.h"
#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "display.h"
#include "face.h"
#include "clock.h"
#include "ui.h"
#include "sound.h"
#include "game.h"
#include "es7210.h"
#include "gauge.h"
#include "carinfo.h"
#include "obd.h"
#include "nav.h"
#include "intro.h"
#include <Preferences.h>
#include <esp_system.h>

static const uint32_t IDLE_TO_CLOCK_MS = 45000;   // how long without touch/sound before the clock shows
static const int BL_FACE = 85, BL_CLOCK = 70, BL_NIGHT = 12;

// ---------------------------------------------------------------- audio pins (ES7210 mic ADC + ES8311 speaker)
#define I2S_PIN_MCK 2
#define I2S_PIN_BCK 48
#define I2S_PIN_WS  38
#define I2S_PIN_DIN 39
#define I2S_PIN_DOUT 47
#define SAMPLE_RATE 16000
#define CST816_ADDR 0x15

static I2SClass i2s;
static es7210_dev_handle_t es7210 = NULL;
static bool micReady = false, spkReady = false;
static volatile float micLevel = 0;            // 0..1, updated every 16 ms by the mic task
static volatile uint32_t beatCount = 0;        // goes up by one on every detected beat / onset
static volatile uint32_t lastBeatMs = 0;
static volatile bool micOn = false;            // laptop asked for the microphone (MIC 1)
static volatile uint32_t micUntil = 0;
static SemaphoreHandle_t txLock = NULL;

// ---- laptop link: binary frames  0xAA 0x55 <type> <len lo> <len hi> <payload> <xor>
//      type 'A' = 16 kHz mono 16-bit PCM,  type 'E' = event text (hello, tap, pet, ...)
static void sendFrame(char type, const uint8_t* data, uint16_t len) {
  uint8_t hdr[5] = {0xAA, 0x55, (uint8_t)type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
  uint8_t x = 0;
  for (uint16_t i = 0; i < len; i++) x ^= data[i];
  if (txLock) xSemaphoreTake(txLock, portMAX_DELAY);
  Serial.write(hdr, 5); Serial.write(data, len); Serial.write(&x, 1);
  if (txLock) xSemaphoreGive(txLock);
}
static void sendEvent(const char* txt) { sendFrame('E', (const uint8_t*)txt, strlen(txt)); }

enum Mode { M_FACE, M_CLOCK, M_GAUGE, M_NAV };
static bool gaugeReady = false;
static int carPage = 0;                        // 0 speed dial, 1 info page
static int speedLimit = 120;                   // km/h warning, 0 = off (SPEEDLIMIT n, kept in flash)
static uint32_t gaugeAwayUntil = 0;            // tapped away from the gauge: face until then
static Mode mode = M_FACE;
static uint32_t lastActivity = 0;
static int clockBl = -1;                       // backlight the clock last asked for
static ClockTime ct = {2026, 1, 1, 0, 0, 0, false};

static void activity() { lastActivity = millis(); }
static void showFace() {
  if (mode != M_FACE) { mode = M_FACE; clockBl = -1; display_set_backlight(BL_FACE); }
  activity();
}
static void react(Emotion e, uint32_t ms, Sound s) {   // face + its sound
  showFace(); face_set_emotion(e, ms); if (spkReady) sound_play(s);
}

// ================================================================= RTC (PCF85063 at 0x51)
static uint8_t bcd(int v) { return ((v / 10) << 4) | (v % 10); }
static int unbcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
static void rtc_set(int y, int mo, int d, int h, int mi, int s) {
  uint8_t r[7] = {bcd(s), bcd(mi), bcd(h), bcd(d), (uint8_t)((weekday_mon0(y, mo, d) + 1) % 7), bcd(mo), bcd(y % 100)};
  I2C_Write(0x51, 0x04, r, 7);
}
static void rtc_init() {                       // run, 24 h, 12.5 pF crystal load (as Waveshare's example)
  uint8_t v = 0x01;
  I2C_Write(0x51, 0x00, &v, 1);
}
static ClockTime rtc_get() {
  ClockTime c = {2026, 1, 1, 0, 0, 0, false};
  uint8_t r[7];
  if (I2C_Read(0x51, 0x04, r, 7)) return c;    // note: this driver returns true on FAILURE
  if (r[0] & 0x80) return c;                   // oscillator stopped = clock lost power / never set
  c.s = unbcd(r[0] & 0x7F); c.mi = unbcd(r[1] & 0x7F); c.h = unbcd(r[2] & 0x3F);
  c.d = unbcd(r[3] & 0x3F); c.mo = unbcd(r[5] & 0x1F); c.y = 2000 + unbcd(r[6]);
  c.valid = c.h < 24 && c.mi < 60 && c.mo >= 1 && c.mo <= 12 && c.d >= 1 && c.d <= 31 && c.y >= 2024;
  return c;
}
// the RTC chip has one spare RAM byte (0x03): we keep "last day we said good morning" there,
// so a reboot doesn't make it say it again
static uint8_t rtc_ram_get() { uint8_t v = 0; if (I2C_Read(0x51, 0x03, &v, 1)) return 0; return v; }
static void rtc_ram_set(uint8_t v) { I2C_Write(0x51, 0x03, &v, 1); }

// ================================================================= microphone
static bool mic_init() {
  es7210_i2c_config_t ic = {};
  ic.i2c_addr = 0x40;
  if (es7210_new_codec(&ic, &es7210) != ESP_OK) return false;
  es7210_codec_config_t cc = {};
  cc.i2s_format = ES7210_I2S_FMT_I2S;
  cc.mclk_ratio = 256;
  cc.sample_rate_hz = SAMPLE_RATE;
  cc.bit_width = ES7210_I2S_BITS_16B;
  cc.mic_bias = ES7210_MIC_BIAS_2V87;
  cc.mic_gain = ES7210_MIC_GAIN_30DB;
  cc.flags.tdm_enable = false;
  if (es7210_config_codec(es7210, &cc) != ESP_OK) return false;
  es7210_config_volume(es7210, 0);
  return true;
}
static bool i2s_init() {
  i2s.setPins(I2S_PIN_BCK, I2S_PIN_WS, I2S_PIN_DOUT, I2S_PIN_DIN, I2S_PIN_MCK);
  i2s.setTimeout(200);
  return i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
}

static volatile uint32_t lastClapMs = 0;
static volatile int clapCount = 0;
static volatile float micPeak = 0, micPeakBg = 0;
static int gFps = 0, gWorst = 0;
static volatile int nDowns = 0, nClaps = 0; static uint32_t clapT[12]; static float clapP[12], clapB[12]; static volatile float maxPeak = 0;   // test counters (TP)              // frames last second, slowest frame (ms)
static void micTask(void*) {
  const int N = 256;
  static int16_t st[N * 2];
  float dcx = 0, dcy = 0, avg = 0;
  uint32_t lastOnset = 0;
  for (;;) {
    size_t got = i2s.readBytes((char*)st, sizeof(st));
    int frames = got / 4;
    if (frames <= 0) { vTaskDelay(2); continue; }
    if (micOn && millis() > micUntil) micOn = false;      // laptop vanished
    if (micOn) {                                           // stream mono to the laptop
      static int16_t mono[N];
      for (int i = 0; i < frames; i++) mono[i] = (int16_t)(((int32_t)st[2 * i] + st[2 * i + 1]) / 2);
      sendFrame('A', (const uint8_t*)mono, frames * 2);
    }
    double sum = 0; float peak = 0;
    for (int i = 0; i < frames; i++) {
      float x = 0.5f * (st[2 * i] + st[2 * i + 1]);
      float y = x - dcx + 0.995f * dcy;          // remove DC offset
      dcx = x; dcy = y; sum += fabsf(y);
      if (fabsf(y) > peak) peak = fabsf(y);
    }
    float lvl = fminf(1.0f, (float)(sum / frames) / 1500.0f);   // quiet room ~0.02, speech ~0.2-1
    if (sound_busy()) lvl = 0;                   // don't listen to our own speaker
    // clap: a sharp jump from quiet to loud
    // (relative to the room's background, so a fan or TV doesn't hide it; re-armed once it drops)
    // uses the sample peak: a clap is a few ms long and vanishes in the average
    { static float pkAvg = 0.02f, prevPk = 0; uint32_t t = millis();
      float pk = sound_busy() ? 0 : fminf(1.0f, peak / 20000.0f);
      micPeak = pk; micPeakBg = pkAvg; if (pk > maxPeak) maxPeak = pk;
      // a clap = sudden jump against the previous 16 ms (ring-out only decays, so it never jumps)
      if (pk > fmaxf(0.30f, pkAvg * 4.0f) && pk > prevPk * 2.5f && t - lastClapMs > 180) {
        lastClapMs = t; clapCount++; clapT[nClaps % 12] = t; clapP[nClaps % 12] = pk; clapB[nClaps % 12] = pkAvg; nClaps++;
      }
      prevPk = pk;
      pkAvg += (pk - pkAvg) * 0.02f; }
    micLevel = lvl;
    // beat / onset detector: energy jumps well above the last ~0.7 s average
    uint32_t now = millis();
    if (lvl > 0.06f && lvl > avg * 1.6f && now - lastOnset > 250) { lastOnset = now; lastBeatMs = now; beatCount++; }
    avg += (lvl - avg) * 0.025f;
  }
}

// ================================================================= sound reactions + dancing
static bool dancing = false;
static bool noisy = false;      // loud steady background (car): no dancing or noise reactions

static void music_update() {
  // keep the last 10 beat times; music = steady beats (similar gaps), speech = irregular
  static uint32_t seen = 0, times[10]; static int nT = 0;
  static int sway = 1;
  uint32_t bc = beatCount, now = millis();
  while (seen != bc) {
    seen++;
    for (int i = 9; i > 0; i--) times[i] = times[i - 1];
    times[0] = lastBeatMs; if (nT < 10) nT++;
    if (dancing) { face_bounce(28); sway = -sway; face_sway(sway * 26); activity(); }
  }
  bool steady = false;
  if (nT >= 7 && now - times[0] < 1200 && now - times[6] < 5000) {
    float g[6], m = 0, v = 0;
    for (int i = 0; i < 6; i++) { g[i] = times[i] - times[i + 1]; m += g[i]; }
    m /= 6;
    for (int i = 0; i < 6; i++) v += (g[i] - m) * (g[i] - m);
    float cv = sqrtf(v / 6) / m;
    // a beat every 0.3-1.1 s with similar gaps
    steady = m > 300 && m < 1100 && cv < 0.32f;
  }
  if (!dancing && steady && !noisy && game_active() == GAME_NONE) {
    dancing = true; showFace(); face_set_emotion(EMO_NEUTRAL, 0); face_set_dance(true);
  }
  if (dancing && now - times[0] > 2500) { dancing = false; face_set_dance(false); face_set_emotion(EMO_HAPPY, 1500); }
}

static void sound_update(float dt) {
  static float slow = 0, loudFor = 0, talkFor = 0;
  static uint32_t lastScare = 0;
  float lvl = micLevel, prev = slow;
  slow += (lvl - slow) * (1.0f - expf(-dt * 3.0f));
  uint32_t now = millis();
  // background noise over ~10 s: high and steady in a moving car (engine, road, wind)
  static float floorLvl = 0;
  floorLvl += (lvl - floorLvl) * (1.0f - expf(-dt / 10.0f));
  noisy = floorLvl > 0.12f && (obd_live() || nav_active());   // only in the car, never at home
  music_update();
  if (noisy && dancing) { dancing = false; face_set_dance(false); }
  if (noisy || dancing || game_active() != GAME_NONE) { clapCount = 0; face_set_attention(false); return; }

  // claps, counted once they stop: 2 kiss, 3 girl, 4+ cool
  if (clapCount > 0 && now - lastClapMs > 900) {
    int n = clapCount; clapCount = 0;
    if (n == 1) { }                              // one loud sound alone is usually not a clap: ignore
    else if (n == 2) { showFace(); face_pose(POSE_KISS, 3500); if (spkReady) sound_play(SND_LOVE); }
    else if (n == 3) { showFace(); face_pose(POSE_GIRL, 6000); if (spkReady) sound_play(SND_HAPPY); }
    else { showFace(); face_pose(POSE_COOL, 5000); if (spkReady) sound_play(SND_WHISTLE); }
    return;
  }
  // a long, very loud noise
  loudFor = (slow > 0.6f) ? loudFor + dt : 0;
  if (loudFor > 0.8f && now - lastScare > 6000) { lastScare = now; loudFor = 0; react(EMO_SCARED, 2500, SND_SCARED); }
  // a short call ("Laboo!"): a burst of voice, then quiet -> turn to you and smile
  static float burst = 0; static uint32_t lastCall = 0;
  if (slow > 0.10f) burst += dt;
  else {
    if (burst > 0.2f && burst < 1.5f && clapCount == 0 && now - lastCall > 4000) {
      lastCall = now; showFace(); face_blink(); react(EMO_HAPPY, 2000, SND_HAPPY);
    }
    burst = 0;
  }
  // someone talking nearby
  talkFor = (slow > 0.10f) ? talkFor + dt : fmaxf(0, talkFor - dt * 2);
  bool talking = talkFor > 0.6f;
  face_set_attention(talking);
  if (talking) { if (mode == M_CLOCK && talkFor > 1.5f) showFace(); else if (mode == M_FACE) activity(); }
}

// ================================================================= time of day: morning / night / boredom
static bool isNight() { return ct.valid && (ct.h >= 22 || ct.h < 5); }
static bool isMorning() { return ct.valid && ct.h >= 6 && ct.h < 11; }

// called on every "hello" from a person (touch, or talking that brings the face back)
static bool greet() {
  if (!ct.valid) return false;
  uint8_t today = (uint8_t)ct.d;
  static int nightSaid = -1;                   // day-of-month of the evening we said good night
  if (isMorning() && rtc_ram_get() != today) {
    rtc_ram_set(today);
    showFace(); face_set_emotion(EMO_HAPPY, 3000); face_caption(TX_MORNING, 3000);
    if (spkReady) sound_play(SND_MORNING);
    return true;
  }
  if (isNight()) {
    int key = ct.h < 5 ? ct.d - 1 : ct.d;      // after midnight it still counts as the same night
    if (nightSaid != key) {
      nightSaid = key;
      showFace(); face_yawn(); face_caption(TX_NIGHT, 3200);
      if (spkReady) sound_play(SND_YAWN);
      return true;
    }
  }
  return false;
}

static uint32_t nextYawn = 0, nextBored = 0, boredUntil = 0;
static void moods_update() {
  uint32_t now = millis();
  // night: yawn now and then while the face is up
  if (mode == M_FACE && isNight() && !dancing && game_active() == GAME_NONE && !face_is_yawning()) {
    if (nextYawn == 0) nextYawn = now + 60000 + random(120000);
    if (now > nextYawn) { face_yawn(); if (spkReady) sound_play(SND_YAWN); nextYawn = now + 90000 + random(150000); }
  }
  // day: bored on the clock -> peek out, look around, whistle, go back
  // now and then, on its own: a wink or the cool glasses (every 3-7 min while the face is idle)
  static uint32_t nextTrick = 0;
  if (nextTrick == 0) nextTrick = now + 180000 + random(240000);
  if (now > nextTrick) {
    nextTrick = now + 180000 + random(240000);
    if (mode == M_FACE && !dancing && game_active() == GAME_NONE && face_get_pose() == POSE_NONE)
    { bool w = random(100) < 65; face_pose(w ? POSE_WINK : POSE_COOL, w ? 1800 : 4000); }
  }
  bool day = ct.valid && ct.h >= 8 && ct.h < 22;
  if (mode == M_CLOCK && day) {
    if (nextBored == 0) nextBored = now + 600000 + random(600000);          // every 10-20 min
    if (now > nextBored) {
      nextBored = now + 600000 + random(600000);
      mode = M_FACE; clockBl = -1; display_set_backlight(BL_FACE);         // (not counted as activity)
      boredUntil = now + 6500;
      face_set_emotion(EMO_NEUTRAL, 0); face_look(-0.9f, 0);
      if (spkReady) sound_play(SND_WHISTLE);
    }
  }
  if (boredUntil) {
    uint32_t left = boredUntil > now ? boredUntil - now : 0;
    if (left < 4500 && left > 2500) face_look(0.9f, 0);
    if (left <= 2500) face_look_release();
    if (left == 0) { boredUntil = 0; if (now - lastActivity > IDLE_TO_CLOCK_MS) mode = M_CLOCK; }
  }
}

// ================================================================= touch + gestures
static bool touch_read(bool& down, int& x, int& y) {
  uint8_t buf[6];
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x01);
  if (Wire.endTransmission(true)) return false;
  if (Wire.requestFrom(CST816_ADDR, 6) != 6) return false;
  for (int i = 0; i < 6; i++) buf[i] = Wire.read();
  down = buf[1] != 0;
  x = ((buf[2] & 0x0F) << 8) | buf[3];
  y = ((buf[4] & 0x0F) << 8) | buf[5];
  return true;
}
static void touch_init() {
  Set_EXIO(EXIO_PIN1, Low);  delay(10);
  Set_EXIO(EXIO_PIN1, High); delay(200);   // chip needs time after reset before it takes settings
  uint8_t v = 1;                       // disable auto sleep so we can poll it
  I2C_Write(CST816_ADDR, 0xFE, &v, 1);
  v = 0x60;                            // report touches and moves
  I2C_Write(CST816_ADDR, 0xFA, &v, 1);
  Wire.beginTransmission(CST816_ADDR);
  blog("[boot] touch %s\n", Wire.endTransmission(true) == 0 ? "OK" : "NOT ANSWERING");
}

static bool tDown = false, consumed = false, panelAsked = false;
static int tx0, ty0, tlx, tly; static float tPath = 0; static uint32_t tStart = 0;
static uint32_t lastTap = 0; static int tapStreak = 0;

static void touch_poll() {
  bool down; int x, y;
  static uint16_t fails = 0;
  if (!touch_read(down, x, y)) {                  // chip fell asleep or hung: reset it
    if (++fails >= 60) { fails = 0; blog("[touch] no answer, resetting\n"); touch_init(); }
    return;
  }
  fails = 0;
  uint32_t now = millis();
  // the speaker disturbs the touch chip and makes fake presses (which looked like a long press
  // and sent the face to the clock): ignore new presses while a sound plays
  // (only the long press is blocked, see below)
  // the chip can drift into a sleep where it answers but never reports a finger:
  // on the clock (nobody touching) wake it up once a minute
  static uint32_t lastKick = 0;
  if (down) lastKick = now;
  else if (mode == M_CLOCK && now - lastKick > 60000) { lastKick = now; touch_init(); return; }
  if (mode == M_GAUGE) {                       // car: tap = next page (dial, info, face); hold on info = clear fault codes
    static bool held = false;
    if (down && !tDown) { tDown = true; held = false; tStart = now; }
    if (down && !held && carPage == 1 && now - tStart > 2000 && obd_data().dtcCount > 0) {
      held = true; obd_clear_codes(); gaugeAwayUntil = now + 4000; react(EMO_HAPPY, 3000, SND_HAPPY);
    }
    if (!down && tDown) {
      tDown = false;
      if (!held && ++carPage > 1) { carPage = 0; gaugeAwayUntil = now + 20000; react(EMO_HAPPY, 2000, SND_HAPPY); }
    }
    return;
  }
  if (mode == M_NAV) {                         // navigating: a tap shows the face for a while
    if (down && !tDown) { tDown = true; consumed = true; gaugeAwayUntil = now + 20000; showFace();
                          react(EMO_HAPPY, 2000, SND_HAPPY); }
    if (!down) tDown = false;
    return;
  }
  if (down && x < LCD_W && y < LCD_H) {
    if (!tDown) {
      Serial.printf("[touch] down %d %d mode %d\n", x, y, (int)mode); nDowns++;
      tDown = true; consumed = false; panelAsked = false; tx0 = tlx = x; ty0 = tly = y; tPath = 0; tStart = now;
      boredUntil = 0;
      if (game_active() != GAME_NONE) { game_tap(); consumed = true; }        // games want the tap at once
      else if (mode == M_CLOCK) {                                               // touching the clock wakes the face
        showFace();
        if (!greet()) react(EMO_HAPPY, 2500, SND_HAPPY);
        face_blink(); consumed = true;
      }
    }
    activity();
    // keep holding 5 s: open the settings panel (Wi-Fi "Laboo")
    if (!panelAsked && now - tStart > 5000 && tPath < 30 && !sound_busy()) {
      panelAsked = true; obd_panel(10); showFace(); react(EMO_SURPRISED, 2500, SND_HAPPY);
      Serial.println("[touch] panel on");
    }
    // long press inside a game: leave it
    if (game_active() != GAME_NONE && now - tStart > 800) { game_stop(); consumed = true; return; }
    if (consumed) return;
    tPath += sqrtf((float)(x - tlx) * (x - tlx) + (float)(y - tly) * (y - tly));
    tlx = x; tly = y;
    face_look((x - 180) / 150.0f, (y - 170) / 150.0f);      // follow the finger
    if (now - tStart > 1000 && tPath < 30 && !sound_busy()) {                 // long press -> clock
      consumed = true; face_look_release();
      mode = M_CLOCK;
    }
    if (tPath > 260) { tPath = 0; sendEvent("pet"); face_pose(POSE_HUG, 3500); if (spkReady) sound_play(SND_LOVE); }        // petting
  } else if (tDown) {
    tDown = false;
    face_look_release();
    if (consumed) return;
    uint32_t dur = now - tStart;
    int dx = tlx - tx0, dy = tly - ty0;
    if (dur < 500 && abs(dx) < 25 && abs(dy) < 25) {          // a tap
      { char b[32]; snprintf(b, sizeof(b), "tap %d %d", tx0, ty0); sendEvent(b); }
      if (greet()) return;
      bool quick = now - lastTap < 450;
      tapStreak = (now - lastTap < 1500) ? tapStreak + 1 : 1;
      lastTap = now;
      if (tapStreak == 2 && quick) { face_pose(POSE_WINK, 1800); if (spkReady) sound_play(SND_POP); }   // double tap: wink
      else if (tapStreak >= 4) { tapStreak = 0; react(EMO_ANGRY, 3000, SND_ANGRY); }
      else if (isNight() && random(100) < 30) { face_yawn(); if (spkReady) sound_play(SND_YAWN); }
      else react(EMO_HAPPY, 2500, SND_HAPPY);
    } else if (dur < 700 && tPath < 260 && (abs(dx) > 70 || abs(dy) > 70)) {   // a swipe
      if (abs(dx) > abs(dy)) face_pose(POSE_ZIP, 5000);          // sideways swipe: "shut up", zip the mouth
      else if (dy < 0) { face_pose(POSE_DIZZY, 3500); if (spkReady) sound_play(SND_SURPRISED); }   // swipe up: dizzy
      else mode = M_CLOCK;                                                                         // swipe down: clock
    }
  }
}

// ================================================================= USB commands (optional)
static void print_resets();
static void handleLine(char* line) {
  char* cmd = strtok(line, " \r\n");
  if (!cmd) return;
  char* a1 = strtok(NULL, " \r\n");
  char* a2 = strtok(NULL, " \r\n");
  bool ok;
  if (!strcmp(cmd, "PING")) { sendEvent(micReady ? "hello mic_ok v3" : "hello mic_fail v3"); Serial.printf("hello laboo-desk mic_%s spk_%s\n", micReady ? "ok" : "fail", spkReady ? "ok" : "fail"); }
  else if (!strcmp(cmd, "MIC") && a1) { micOn = atoi(a1) != 0; micUntil = millis() + 30000; }
  else if (!strcmp(cmd, "M") && a1) { face_set_mouth(atoi(a1)); }
  else if (!strcmp(cmd, "L") && a1) {
    if (!strcmp(a1, "off")) face_look_release();
    else if (a2) { showFace(); face_look(atoi(a1) / 100.0f, atoi(a2) / 100.0f); }
  }
  else if (!strcmp(cmd, "INFO")) { Serial.print(blog_text()); }
  else if (!strcmp(cmd, "NOW")) {
    ClockTime c = rtc_get(); int jy, jm, jd; to_jalali(c.y, c.mo, c.d, jy, jm, jd);
    Serial.printf("now %s %04d-%02d-%02d %02d:%02d:%02d  (jalali %d/%02d/%02d)\n", c.valid ? "ok" : "bad",
                  c.y, c.mo, c.d, c.h, c.mi, c.s, jy, jm, jd);
  }
  else if (!strcmp(cmd, "TIME") && a1 && a2) {
    int v[6] = {atoi(a1), atoi(a2), 0, 0, 0, 0};
    for (int i = 2; i < 6; i++) { char* x = strtok(NULL, " \r\n"); v[i] = x ? atoi(x) : 0; }
    rtc_set(v[0], v[1], v[2], v[3], v[4], v[5]);
    ClockTime c = rtc_get();
    Serial.printf("time %s %04d-%02d-%02d %02d:%02d:%02d\n", c.valid ? "ok" : "bad", c.y, c.mo, c.d, c.h, c.mi, c.s);
  }
  else if (!strcmp(cmd, "E") && a1) { Emotion e = emotion_from_name(a1, &ok); if (ok) { showFace(); face_set_emotion(e, a2 ? atoi(a2) : 0); } }
  else if (!strcmp(cmd, "S") && a1) { FaceState s = state_from_name(a1, &ok); if (ok) { showFace(); face_set_state(s); } }
  else if (!strcmp(cmd, "SND") && a1) { sound_play((Sound)constrain(atoi(a1), 0, SND_COUNT - 1)); }
  else if (!strcmp(cmd, "VOL") && a1) { sound_set_volume(atoi(a1)); }
  else if (!strcmp(cmd, "GIRL"))  { showFace(); face_pose(POSE_GIRL, 6000); }
  else if (!strcmp(cmd, "KISS"))  { showFace(); face_pose(POSE_KISS, 3500); }
  else if (!strcmp(cmd, "ZIP"))   { showFace(); face_pose(POSE_ZIP, 5000); }
  else if (!strcmp(cmd, "WINK"))  { showFace(); face_pose(POSE_WINK, 1800); }
  else if (!strcmp(cmd, "COOL"))  { showFace(); face_pose(POSE_COOL, 5000); }
  else if (!strcmp(cmd, "HUG"))   { showFace(); face_pose(POSE_HUG, 3500); }
  else if (!strcmp(cmd, "DIZZY")) { showFace(); face_pose(POSE_DIZZY, 3500); }
  else if (!strcmp(cmd, "YAWN"))  { showFace(); face_yawn(); face_caption(TX_NIGHT, 3200); sound_play(SND_YAWN); }
  else if (!strcmp(cmd, "MORNING")) { showFace(); face_set_emotion(EMO_HAPPY, 3000); face_caption(TX_MORNING, 3000); sound_play(SND_MORNING); }
  else if (!strcmp(cmd, "BORED")) { mode = M_CLOCK; nextBored = millis(); }
  else if (!strcmp(cmd, "CLOCK")) { mode = M_CLOCK; }
  else if (!strcmp(cmd, "FACE")) { showFace(); }
  else if (!strcmp(cmd, "CLAPS")) {               // last claps: time (ms) / peak / background
    for (int i = nClaps > 12 ? nClaps - 12 : 0; i < nClaps; i++) Serial.printf("%lu/%.2f/%.2f ", (unsigned long)clapT[i % 12], clapP[i % 12], clapB[i % 12]);
    Serial.printf(" busy=%d\n", sound_busy());
  }
  else if (!strcmp(cmd, "TP")) {                  // touch + sound debug
    bool d; int x, y; bool ok = touch_read(d, x, y);
    Serial.printf("touch %s down=%d x=%d y=%d mode=%d mic=%.3f noisy=%d peak=%.2f/%.2f fps=%d worst=%d downs=%d claps=%d maxpeak=%.2f clock %04d-%02d-%02d %02d:%02d:%02d valid=%d\n", ok ? "ok" : "FAIL", d, x, y, (int)mode, micLevel, noisy, micPeak, micPeakBg, gFps, gWorst, nDowns, nClaps, maxPeak,
                  ct.y, ct.mo, ct.d, ct.h, ct.mi, ct.s, ct.valid);
  }
  else if (!strcmp(cmd, "B") && a1) { display_set_backlight(atoi(a1)); }
  else if (!strcmp(cmd, "GAUGE")) {
    if (a1 && !strcmp(a1, "OFF")) { gauge_demo(false); showFace(); }
    else if (gaugeReady) { mode = M_GAUGE; gauge_demo(a1 && !strcmp(a1, "DEMO")); }
  }
  else if (!strcmp(cmd, "OBD")) { obd_status(Serial); }
  else if (!strcmp(cmd, "CARPAGE")) { carPage = a1 ? atoi(a1) & 1 : !carPage; }
  else if (!strcmp(cmd, "SPEEDLIMIT") && a1) {
    speedLimit = constrain(atoi(a1), 0, 250);
    Preferences p; p.begin("car", false); p.putInt("limit", speedLimit); p.end();
    Serial.printf("speed warning at %d km/h\n", speedLimit);
  }
  else if (!strcmp(cmd, "RESETS")) { print_resets(); }
  else if (!strcmp(cmd, "INTRO") && a1) {          // INTRO 3  (play it)  |  INTRO SET 3  (use it at every start)
    if (!strcmp(a1, "SET") && a2) { intro_save(constrain(atoi(a2), 1, INTRO_COUNT)); Serial.printf("intro %d saved\n", intro_saved()); }
    else { intro_play(constrain(atoi(a1), 1, INTRO_COUNT), BL_FACE); showFace(); }
  }
  else if (!strcmp(cmd, "HOMEWIFI") && a1) {       // HOMEWIFI <ssid> [password]  -> clock from the internet at home
    obd_set_home_wifi(a1, a2 ? a2 : ""); Serial.printf("home wifi set to %s\n", a1);
  }
  else if (!strcmp(cmd, "NAV") && a1) {           // NAV N,R,350,0  |  NAV X   (same text the phone sends)
    nav_handle(a1); Serial.printf("nav %s ble=%d\n", nav_active() ? "on" : "off", nav_connected());
  }
  else if (!strcmp(cmd, "PANEL")) { obd_panel(a1 ? atoi(a1) : 10); Serial.println("panel on: Wi-Fi Laboo / laboo1234, http://192.168.4.1"); }
  else if (!strcmp(cmd, "OBDCLEAR")) { obd_clear_codes(); Serial.println("obd clearing codes"); }
  else if (!strcmp(cmd, "OBDWIFI") && a1) {        // OBDWIFI <ssid> [password|-] [ip] [port]
    char* ip = strtok(NULL, " \r\n"); char* pt = strtok(NULL, " \r\n");
    obd_set_wifi(a1, (a2 && strcmp(a2, "-")) ? a2 : "", ip, pt ? atoi(pt) : 0);
    Serial.printf("obd wifi set to %s\n", a1);
  }
}

// ================================================================= car (OBD)
static void car_update() {
  static bool wasLive = false, hot = false;
  static int lastDtc = 0;
  const ObdData& o = obd_data();
  bool live = obd_live();
  uint32_t now = millis();
  if (live) {
    gauge_set(o.kmh < 0 ? 0 : o.kmh, o.rpm < 0 ? 0 : o.rpm, o.coolant > -1000 ? o.coolant : -1);
    if (!wasLive) blog("[car] OBD live\n");
    // engine running -> gauge (unless the face was just asked for, or a game is on)
    if (gaugeReady && mode != M_GAUGE && mode != M_NAV && !nav_active() && now > gaugeAwayUntil && game_active() == GAME_NONE) mode = M_GAUGE;
    // too hot: scared once
    if (o.coolant >= 105 && !hot) { hot = true; showFace(); gaugeAwayUntil = now + 6000; react(EMO_SCARED, 5000, SND_SCARED); }
    if (o.coolant < 100) hot = false;
    // new fault codes: confused face
    if (o.dtcCount > lastDtc) { showFace(); gaugeAwayUntil = now + 6000; react(EMO_CONFUSED, 5000, SND_SURPRISED); }
    if (o.dtcCount >= 0) lastDtc = o.dtcCount;
    // warnings, each once until the value recovers: too fast, tank nearly empty, battery not charging
    static bool fast = false, lowFuel = false, lowV = false;
    if (speedLimit > 0 && o.kmh > speedLimit && !fast) { fast = true; gaugeAwayUntil = now + 4000; react(EMO_SCARED, 3000, SND_SCARED); }
    if (o.kmh < speedLimit - 5) fast = false;
    if (o.fuelPct >= 0 && o.fuelPct < 12 && !lowFuel) { lowFuel = true; gaugeAwayUntil = now + 5000; react(EMO_SAD, 4000, SND_SURPRISED); }
    if (o.fuelPct > 20) lowFuel = false;
    if (o.rpm > 500 && o.volts > 0 && o.volts < 12.2f && !lowV) { lowV = true; gaugeAwayUntil = now + 5000; react(EMO_CONFUSED, 4000, SND_SURPRISED); }
    if (o.volts > 13.0f) lowV = false;
  } else if (wasLive && mode == M_GAUGE && !gauge_is_demo()) {
    showFace();                                   // engine off -> back to the face
  }
  wasLive = live;

  // navigation from the phone: has priority over the gauge
  static bool navWas = false; static int lastM = 0;
  bool nav = nav_active();
  if (nav) {
    if (mode != M_NAV && now > gaugeAwayUntil && game_active() == GAME_NONE) { mode = M_NAV; display_set_backlight(BL_FACE); }
    int m = nav_metres();
    if (lastM > 200 && m <= 200 && nav_turn() != NT_STRAIGHT && spkReady) sound_play(SND_SURPRISED);  // turn coming up
    if (nav_turn() == NT_ARRIVE && m < 30 && lastM >= 30) { showFace(); gaugeAwayUntil = now + 8000; react(EMO_HAPPY, 4000, SND_HAPPY); }
    lastM = m;
  } else if (navWas && mode == M_NAV) {
    if (live && gaugeReady) mode = M_GAUGE; else showFace();
  }
  navWas = nav;
  if (mode == M_GAUGE) gauge_set_time(ct.valid ? ct.h : -1, ct.mi);
}
static void serial_poll() {
  static char buf[96]; static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { buf[n] = 0; handleLine(buf); n = 0; }
    else if (n < (int)sizeof(buf) - 1) buf[n++] = c;
  }
}

// ================================================================= why did it restart? (kept in flash)
// 1 power on, 3 software, 4 crash, 5/6/7 watchdog, 9 brownout (USB power too weak), 8 deep sleep
static void log_reset_reason() {
  int r = (int)esp_reset_reason();
  Preferences p; p.begin("boot", false);
  uint32_t n = p.getUInt("n", 0) + 1;
  char key[4]; snprintf(key, sizeof(key), "r%u", (unsigned)(n % 10));
  p.putUInt("n", n); p.putUChar(key, (uint8_t)r);
  if (r == ESP_RST_BROWNOUT) p.putUInt("brown", p.getUInt("brown", 0) + 1);
  p.end();
  blog("[boot] start #%lu, reason %d%s""\n", (unsigned long)n, r, r == ESP_RST_BROWNOUT ? " (BROWNOUT: weak USB power)" : "");
}
static void print_resets() {
  Preferences p; p.begin("boot", true);
  uint32_t n = p.getUInt("n", 0);
  Serial.printf("starts %lu, brownouts %lu, last reasons:", (unsigned long)n, (unsigned long)p.getUInt("brown", 0));
  for (uint32_t i = (n > 9 ? n - 9 : 1); i <= n; i++) { char k[4]; snprintf(k, sizeof(k), "r%u", (unsigned)(i % 10)); Serial.printf(" %d", p.getUChar(k, 0)); }
  Serial.println();
  p.end();
}

// ================================================================= main
void setup() {
  Serial.begin(921600);
  txLock = xSemaphoreCreateMutex();
  I2C_Init();                    // SDA 11, SCL 10
  Wire.setClock(400000);
  TCA9554PWR_Init(0x00);
  Set_EXIOS(0xFF);
  bool dok = display_init();
  log_reset_reason();
  blog("[boot] LABOO desk v3.1, display %s\n", dok ? "OK" : "FAILED");
  face_init();
  touch_init();
  micReady = mic_init();
  spkReady = sound_codec_init();
  bool i2sOk = i2s_init();
  if (!i2sOk) { micReady = spkReady = false; }
  blog("[boot] mic %s, speaker %s\n", micReady ? "OK" : "FAILED", spkReady ? "OK" : "FAILED");
  if (micReady) xTaskCreatePinnedToCore(micTask, "mic", 6144, NULL, 10, NULL, 0);
  if (spkReady) sound_start(&i2s);
  rtc_init();
  ct = rtc_get();
  blog("[boot] clock %s\n", ct.valid ? "set" : "NOT SET (send TIME over USB)");
  clock_set(ct);
  gaugeReady = gauge_init();
  blog("[boot] gauge %s\n", gaugeReady ? "OK" : "FAILED");
  { Preferences p; p.begin("car", true); speedLimit = p.getInt("limit", 120); p.end(); }
  obd_begin();
  if (!ct.valid) obd_panel(10);                 // no clock: open the "Laboo" Wi-Fi panel so a phone/PC can set it
  nav_begin();
  blog("[boot] ble nav ready\n");
  intro_play(intro_saved(), BL_FACE);           // cinematic start (INTRO n to try one, INTRO SET n to keep it)
  if (!greet()) { face_set_emotion(EMO_HAPPY, 2500); if (spkReady) sound_play(SND_HAPPY); }   // hello!
  activity();
}

void loop() {
  static uint32_t last = millis(), lastRtc = 0;
  uint32_t now = millis();
  float dt = (now - last) / 1000.0f; last = now;
  if (dt > 0.1f) dt = 0.1f;

  serial_poll();
  touch_poll();
  if (now - lastRtc > 1000) { lastRtc = now; ct = rtc_get(); clock_set(ct); }
  if (micReady) sound_update(dt);
  moods_update();
  game_update(dt);
  car_update();
  { struct tm t; if (obd_take_time(&t)) { rtc_set(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec); ct = rtc_get(); clock_set(ct); blog("[clock] set from the internet\n"); } }
  { int v[6]; if (nav_take_time(v)) { rtc_set(v[0], v[1], v[2], v[3], v[4], v[5]); ct = rtc_get(); clock_set(ct); } }

  bool busy = dancing || boredUntil || game_active() != GAME_NONE || face_get_state() == ST_SPEAKING;
  if (mode == M_FACE && !busy && now - lastActivity > IDLE_TO_CLOCK_MS) mode = M_CLOCK;

  if (mode == M_NAV) {
    gauge_update(dt);
    nav_render_frame(obd_data().kmh, obd_live());
    face_update(dt);
  } else if (mode == M_GAUGE) {
    gauge_update(dt);
    if (carPage == 1) carinfo_render_frame(gauge_is_demo()); else gauge_render_frame();
    face_update(dt);
  } else if (mode == M_CLOCK && game_active() == GAME_NONE) {
    bool night = ct.valid && (ct.h >= 23 || ct.h < 7);
    int want = night ? BL_NIGHT : BL_CLOCK;
    if (clockBl != want) { clockBl = want; display_set_backlight(want); }
    clock_set_dim(night);
    clock_update(dt);
    clock_render_frame();
    face_update(dt);                           // keep the face's timers ticking
  } else {
    face_update(dt);
    if (game_draws_face()) face_render_frame(); else game_render_frame();
  }

  uint32_t spent = millis() - now;
  { static uint32_t t0 = 0; static int n = 0, worst = 0; n++; if ((int)spent > worst) worst = spent;
    if (now - t0 >= 1000) { gFps = n; gWorst = worst; n = 0; worst = 0; t0 = now; } }
  if (spent < 30) delay(30 - spent);           // ~30 fps
}
