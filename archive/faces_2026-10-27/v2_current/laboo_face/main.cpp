/*
  LABOO - robot face firmware
  Board : Waveshare ESP32-S3-Touch-LCD-1.85C
  Role  : shows an animated face, reads touch, streams the microphone to the
          laptop over USB and obeys commands coming back from the laptop.

  Arduino IDE settings (Tools menu):
    Board            : ESP32S3 Dev Module         (esp32 core by Espressif >= 3.0.2)
    USB CDC On Boot  : Enabled
    USB Mode         : Hardware CDC and JTAG
    Flash Size       : 16MB (128Mb)
    PSRAM            : OPI PSRAM
    Partition Scheme : 16M Flash (3MB APP/9.9MB FATFS)
    Core Debug Level : None

  ---- USB protocol ----
  Laptop -> board : text lines
      PING                      -> board answers with event "hello"
      E <emotion> [ms]          neutral happy sad angry surprised love confused sleepy
      S <state>                 idle listening thinking speaking sleeping
      M <0..100>                mouth opening (lip sync)
      L <x> <y> | L off         look direction, -100..100
      B <0..100>                backlight
      MIC 1 | MIC 0             start / stop streaming the microphone
      IMG <bytes>  + raw data   show a full-screen picture (RLE: [count-1][hi][lo] per run, RGB565)
      FACE                      back from a picture / gauge to the animated face
      GAUGE [DEMO|OFF]          car gauge (DEMO = fake drive to see it move)
      V <kmh> <rpm> <coolant>   live values for the gauge
      TIME <y> <mo> <d> <h> <mi> <s>   set the clock (kept by the RTC chip, used in the car)
  With no computer on the USB (plain charger in the car) the gauge comes up by itself.
  Board -> laptop : binary frames  0xAA 0x55 <type> <len lo> <len hi> <payload> <xor>
      type 'A' = 16 kHz mono 16-bit PCM,  type 'E' = event text (tap, long, pet, swipe_left, ...)
*/
#include <Arduino.h>
#include <Wire.h>
#include "ESP_I2S.h"
#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "display.h"
#include "face.h"
#include "es7210.h"
#include "gauge.h"

// ---------------------------------------------------------------- audio pins (ES7210 mic ADC)
#define I2S_PIN_MCK 2
#define I2S_PIN_BCK 48
#define I2S_PIN_WS  38
#define I2S_PIN_DIN 39
#define I2S_PIN_DOUT 47
#define SAMPLE_RATE 16000

// ---------------------------------------------------------------- touch (CST816)
#define CST816_ADDR 0x15

static I2SClass i2s;
static es7210_dev_handle_t es7210 = NULL;
static SemaphoreHandle_t txLock;
static volatile bool micOn = false;
static volatile uint32_t micUntil = 0;
static bool micReady = false;

// ---------------------------------------------------------------- picture mode (laptop draws the screen)
static uint16_t* screenBuf = NULL;            // 360x360 in PSRAM, bytes already in panel order
static uint8_t*  imgRx = NULL;                // compressed picture being received
static const uint32_t IMG_MAX = 400000;
static volatile bool screenMode = false, screenDirty = false;
static bool gaugeMode = false, gaugeAuto = false, gaugeReady = false;

// ---------------------------------------------------------------- RTC (PCF85063 at 0x51)
static uint8_t bcd(int v) { return ((v / 10) << 4) | (v % 10); }
static int unbcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
static void rtc_set(int y, int mo, int d, int h, int mi, int s) {
  uint8_t r[7] = {bcd(s), bcd(mi), bcd(h), bcd(d), 0, bcd(mo), bcd(y % 100)};
  I2C_Write(0x51, 0x04, r, 7);
}
static bool rtc_get(int& h, int& mi) {
  uint8_t r[3];
  if (!I2C_Read(0x51, 0x04, r, 3)) return false;
  if (r[0] & 0x80) return false;               // clock was never set / lost power
  mi = unbcd(r[1] & 0x7F); h = unbcd(r[2] & 0x3F);
  return h < 24 && mi < 60;
}

// ================================================================= framing
static void sendFrame(char type, const uint8_t* data, uint16_t len) {
  uint8_t hdr[5] = {0xAA, 0x55, (uint8_t)type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
  uint8_t x = 0;
  for (uint16_t i = 0; i < len; i++) x ^= data[i];
  xSemaphoreTake(txLock, portMAX_DELAY);
  Serial.write(hdr, 5);
  Serial.write(data, len);
  Serial.write(&x, 1);
  xSemaphoreGive(txLock);
}
static void sendEvent(const char* txt) { sendFrame('E', (const uint8_t*)txt, strlen(txt)); }

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
  i2s.setPins(I2S_PIN_BCK, I2S_PIN_WS, I2S_PIN_DOUT, I2S_PIN_DIN, I2S_PIN_MCK);
  i2s.setTimeout(200);
  return i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
}

// Reads the mic continuously. Reading and USB sending run in two separate tasks
// joined by a 1-second ring buffer, so a slow USB write can never make the I2S
// DMA overflow (that used to drop a few samples every block = clicks every 32 ms).
static volatile float statLo = 1e9, statHi = 0, statL = 0, statR = 0;
static volatile uint32_t statDrop = 0;
struct MicBlock { int16_t s[256]; uint16_t n; };
static QueueHandle_t micQueue = NULL;         // whole blocks only -> samples can never get misaligned

static void micTask(void*) {
  const int N = 256;                         // frames per read (16 ms)
  static int16_t st[N * 2];
  static int16_t mono[N];
  float eL = 0, eR = 0;
  float dcx = 0, dcy = 0;                    // DC blocker state
  int ch = 0; uint32_t lastPick = 0;
  for (;;) {
    size_t got = i2s.readBytes((char*)st, sizeof(st));
    int frames = got / 4;
    if (frames <= 0) { vTaskDelay(2); continue; }
    double sl = 0, sr = 0;
    for (int i = 0; i < frames; i++) { sl += fabsf((float)st[2 * i]); sr += fabsf((float)st[2 * i + 1]); }
    eL = eL * 0.98f + sl / frames * 0.02f;
    eR = eR * 0.98f + sr / frames * 0.02f;
    if (millis() - lastPick > 5000) {        // choose the mic channel rarely (no flip-flopping)
      int want = (eR > eL * 1.5f) ? 1 : (eL > eR * 1.5f ? 0 : ch);
      ch = want; lastPick = millis();
    }
    double sum = 0;
    for (int i = 0; i < frames; i++) {
      float x = st[2 * i + ch];
      float y = x - dcx + 0.995f * dcy;      // removes any DC offset of the mic
      dcx = x; dcy = y;
      if (y > 32767) y = 32767; if (y < -32768) y = -32768;
      mono[i] = (int16_t)y; sum += fabsf(y);
    }
    float lvl = (float)(sum / frames);
    if (lvl < statLo) statLo = lvl;
    if (lvl > statHi) statHi = lvl;
    statL = eL; statR = eR;
    face_set_mic_level(fminf(1.0f, lvl / 1500.0f));   // quiet room ~25, normal speech ~300-2000
    if (micOn && millis() > micUntil) micOn = false;      // safety: laptop vanished
    if (micOn) {
      static MicBlock blk;
      memcpy(blk.s, mono, frames * 2); blk.n = frames;
      if (xQueueSend(micQueue, &blk, 0) != pdTRUE) statDrop += frames;
    }
  }
}

static void micSendTask(void*) {
  static MicBlock blk;
  for (;;) {
    if (xQueueReceive(micQueue, &blk, pdMS_TO_TICKS(100)) == pdTRUE && micOn)
      sendFrame('A', (const uint8_t*)blk.s, blk.n * 2);
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
  Set_EXIO(EXIO_PIN1, High); delay(50);
  uint8_t v = 1;                       // disable auto sleep so we can poll it
  I2C_Write(CST816_ADDR, 0xFE, &v, 1);
}

static bool tDown = false, longSent = false;
static int tx0, ty0, tlx, tly; static float tPath = 0; static uint32_t tStart = 0;

static void touch_poll() {
  bool down; int x, y;
  if (!touch_read(down, x, y)) return;
  uint32_t now = millis();
  if (down && x < LCD_W && y < LCD_H) {
    if (!tDown) { tDown = true; longSent = false; tx0 = tlx = x; ty0 = tly = y; tPath = 0; tStart = now; face_poke(); }
    tPath += sqrtf((float)(x - tlx) * (x - tlx) + (float)(y - tly) * (y - tly));
    tlx = x; tly = y;
    face_look((x - 180) / 150.0f, (y - 170) / 150.0f);      // follow the finger
    if (!longSent && now - tStart > 900 && tPath < 30) {
      longSent = true; sendEvent("long"); face_set_emotion(EMO_SURPRISED, 1200);
      if (gaugeMode) gauge_demo(!gauge_is_demo());
    }
    if (tPath > 260) { tPath = 0; sendEvent("pet"); face_set_emotion(EMO_LOVE, 4000); }
  } else if (tDown) {
    tDown = false;
    face_look_release();
    uint32_t dur = now - tStart;
    int dx = tlx - tx0, dy = tly - ty0;
    if (longSent) return;
    if (dur < 400 && abs(dx) < 25 && abs(dy) < 25) {
      char b[32]; snprintf(b, sizeof(b), "tap %d %d", tx0, ty0); sendEvent(b);
      face_blink();
    } else if (dur < 700 && (abs(dx) > 70 || abs(dy) > 70)) {
      if (abs(dx) > abs(dy)) sendEvent(dx > 0 ? "swipe_right" : "swipe_left");
      else sendEvent(dy > 0 ? "swipe_down" : "swipe_up");
    }
  }
}

// ================================================================= pictures from laptop
// Reads <len> raw bytes right after the "IMG <len>" line, unpacks them into the screen buffer.
static void receive_image(uint32_t len) {
  if (!screenBuf || !imgRx || len == 0 || len > IMG_MAX) { sendEvent("img_bad"); return; }
  uint32_t got = 0, t0 = millis();
  while (got < len && millis() - t0 < 3000) {
    int a = Serial.available();
    if (a <= 0) { delay(1); continue; }
    if ((uint32_t)a > len - got) a = len - got;
    got += Serial.readBytes(imgRx + got, a);
    t0 = millis();
  }
  if (got < len) { sendEvent("img_timeout"); return; }
  uint8_t* px = (uint8_t*)screenBuf;
  const uint32_t total = LCD_W * LCD_H;
  uint32_t p = 0;
  for (uint32_t i = 0; i + 2 < len + 0 && p < total; i += 3) {
    uint32_t n = imgRx[i] + 1;
    uint8_t hi = imgRx[i + 1], lo = imgRx[i + 2];
    while (n-- && p < total) { px[2 * p] = hi; px[2 * p + 1] = lo; p++; }
  }
  while (p < total) { px[2 * p] = 0; px[2 * p + 1] = 0; p++; }
  screenMode = true; screenDirty = true;
  if (!gaugeAuto) gaugeMode = false;      // a picture from the laptop replaces a gauge it asked for
  sendEvent("img_ok");
}

static void screen_push() {
  for (int y0 = 0; y0 < LCD_H; y0 += BAND_H) {
    uint16_t* b = display_band_buffer();
    memcpy(b, screenBuf + y0 * LCD_W, LCD_W * BAND_H * 2);
    display_push_band(y0, BAND_H);
  }
}

// ================================================================= commands from laptop
static void receive_image(uint32_t len);
static void handleLine(char* line) {
  char* cmd = strtok(line, " \r\n");
  if (!cmd) return;
  char* a1 = strtok(NULL, " \r\n");
  char* a2 = strtok(NULL, " \r\n");
  bool ok;
  if (!strcmp(cmd, "INFO")) {
    xSemaphoreTake(txLock, portMAX_DELAY); Serial.print("\n"); Serial.print(blog_text()); xSemaphoreGive(txLock);
  }
  else if (!strcmp(cmd, "PING")) { sendEvent(micReady ? "hello mic_ok v2 img" : "hello mic_fail v2 img"); }
  else if (!strcmp(cmd, "FACE")) { screenMode = false; gaugeMode = false; gaugeAuto = false; gauge_demo(false); }
  else if (!strcmp(cmd, "GAUGE")) {
    if (a1 && !strcmp(a1, "OFF")) { gaugeMode = false; gauge_demo(false); }
    else if (gaugeReady) { screenMode = false; gaugeMode = true; gaugeAuto = false; gauge_demo(a1 && !strcmp(a1, "DEMO")); }
  }
  else if (!strcmp(cmd, "V") && a1 && a2) {
    char* a3 = strtok(NULL, " \r\n");
    gauge_set(atof(a1), atof(a2), a3 ? atoi(a3) : -1);
  }
  else if (!strcmp(cmd, "TIME") && a1 && a2) {
    int v[6] = {atoi(a1), atoi(a2), 0, 0, 0, 0};
    for (int i = 2; i < 6; i++) { char* x = strtok(NULL, " \r\n"); v[i] = x ? atoi(x) : 0; }
    rtc_set(v[0], v[1], v[2], v[3], v[4], v[5]);
  }
  else if (!strcmp(cmd, "IMG") && a1) { receive_image((uint32_t)atol(a1)); }
  else if (!strcmp(cmd, "E") && a1) { Emotion e = emotion_from_name(a1, &ok); if (ok) face_set_emotion(e, a2 ? atoi(a2) : 0); }
  else if (!strcmp(cmd, "S") && a1) { FaceState s = state_from_name(a1, &ok); if (ok) face_set_state(s); }
  else if (!strcmp(cmd, "M") && a1) { face_set_mouth(atoi(a1)); }
  else if (!strcmp(cmd, "L") && a1) {
    if (!strcmp(a1, "off")) face_look_release();
    else if (a2) face_look(atoi(a1) / 100.0f, atoi(a2) / 100.0f);
  }
  else if (!strcmp(cmd, "B") && a1) { display_set_backlight(atoi(a1)); }
  else if (!strcmp(cmd, "MIC") && a1) {
    micOn = atoi(a1) != 0;
    micUntil = millis() + 30000;
  }
}

static void serial_poll() {
  static char buf[96]; static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { buf[n] = 0; handleLine(buf); n = 0; }
    else if (n < (int)sizeof(buf) - 1) buf[n++] = c;
  }
}

// ================================================================= main
void setup() {
#if ARDUINO_USB_MODE
  Serial.setTxBufferSize(8192);
  Serial.setRxBufferSize(16384);
#endif
  Serial.begin(921600);
  txLock = xSemaphoreCreateMutex();

  delay(1500);                   // give the laptop time to open the port and see the boot log
  blog("\n[boot] LABOO starting, PSRAM %u bytes\n", (unsigned)ESP.getPsramSize());
  I2C_Init();                    // SDA 11, SCL 10
  Wire.setClock(400000);
  TCA9554PWR_Init(0x00);         // all expander pins = outputs
  Set_EXIOS(0xFF);

  blog("[boot] expander ok\n");
  bool dok = display_init();
  blog("[boot] display_init -> %s\n", dok ? "OK" : "FAILED");
  // colour test: red, green, blue, white
  const uint16_t test[4] = {0x00F8, 0xE007, 0x1F00, 0xFFFF};
  for (int i = 0; i < 4; i++) { display_fill(test[i]); delay(400); }
  face_init();
  screenBuf = (uint16_t*)heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
  imgRx = (uint8_t*)heap_caps_malloc(IMG_MAX, MALLOC_CAP_SPIRAM);
  blog("[boot] picture buffers %s\n", (screenBuf && imgRx) ? "OK" : "FAILED");
  gaugeReady = gauge_init();
  blog("[boot] gauge %s\n", gaugeReady ? "OK" : "FAILED");
  touch_init();
  blog("[boot] touch ok\n");
  micReady = mic_init();
  blog("[boot] mic %s\n", micReady ? "OK" : "FAILED");
  if (micReady) {
    micQueue = xQueueCreate(64, sizeof(MicBlock));      // ~1 s of audio
    xTaskCreatePinnedToCore(micTask, "mic", 8192, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(micSendTask, "micsend", 4096, NULL, 4, NULL, 0);
  }
  face_set_emotion(EMO_HAPPY, 2500);
}

void loop() {
  static uint32_t last = millis();
  uint32_t now = millis();
  float dt = (now - last) / 1000.0f; last = now;
  if (dt > 0.1f) dt = 0.1f;

  serial_poll();
  touch_poll();
  // computer on the USB -> robot face; plain charger (car) -> gauge
  static uint32_t lastHost = 0, lastClock = 0;
  if (HWCDC::isPlugged() || now < 4000) lastHost = now;
  if (gaugeReady && !gaugeMode && now - lastHost > 2500) { gaugeMode = true; gaugeAuto = true; screenMode = false; }
  if (gaugeMode && gaugeAuto && now - lastHost < 100) { gaugeMode = false; gaugeAuto = false; }
  if (gaugeMode && now - lastClock > 1000) {
    lastClock = now;
    int h, mi;
    if (rtc_get(h, mi)) gauge_set_time(h, mi);
  }

  face_update(dt);
  if (gaugeMode) {
    gauge_update(dt);
    gauge_render_frame();
  } else if (screenMode) {
    if (screenDirty) { screenDirty = false; screen_push(); }
  } else {
    face_render_frame();
  }

  static uint32_t frames = 0, lastRep = 0;
  frames++;
  if (now - lastRep > 5000 && !micOn) {
    char b[128]; snprintf(b, sizeof(b), "\n[alive] fps %.1f | mic quiet %.0f loud %.0f | chL %.0f chR %.0f | drop %u\n",
                          frames * 1000.0f / (now - lastRep), statLo, statHi, statL, statR, (unsigned)statDrop);
    statLo = 1e9; statHi = 0;
    xSemaphoreTake(txLock, portMAX_DELAY); Serial.print(b); xSemaphoreGive(txLock);
    frames = 0; lastRep = now;
  }
  uint32_t spent = millis() - now;
  if (spent < 30) delay(30 - spent);      // ~30 fps
}
