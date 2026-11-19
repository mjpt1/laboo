#include "nav.h"
#include "display.h"
#include "gauge_assets.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <math.h>

#define NAV_SVC "6e400001-1a8b-4f2e-9b6c-1ab0b0ba0001"
#define NAV_RX  "6e400002-1a8b-4f2e-9b6c-1ab0b0ba0001"   // phone -> board (write)

static volatile bool connected = false;
static volatile uint32_t lastMsg = 0;
static volatile bool ended = true;
static volatile int metres = 0, exitNo = 0;
static volatile NavTurn turn = NT_STRAIGHT;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
// route line ahead, in screen pixels from the car (x right, y up), heading-up
#define MAXP 40
static int16_t rpx[MAXP], rpy[MAXP]; static volatile int rpn = 0; static volatile uint32_t lastPath = 0;
static char pbuf[400]; static int plen = 0;

static void parse_path(char* s) {                 // "P,x,y,x,y,...;"
  int16_t xs[MAXP], ys[MAXP]; int n = 0;
  char* save = NULL; strtok_r(s, ",;", &save);
  for (char* a; n < MAXP && (a = strtok_r(NULL, ",;", &save)); ) {
    char* b = strtok_r(NULL, ",;", &save); if (!b) break;
    xs[n] = atoi(a); ys[n] = atoi(b); n++;
  }
  portENTER_CRITICAL(&mux);
  memcpy(rpx, xs, n * 2); memcpy(rpy, ys, n * 2); rpn = n; lastPath = millis();
  portEXIT_CRITICAL(&mux);
}

// ------------------------------------------------------------------ messages
static NavTurn parse_turn(const char* t) {
  if (!strcmp(t, "SL")) return NT_SLIGHT_L;
  if (!strcmp(t, "SR")) return NT_SLIGHT_R;
  if (!strcmp(t, "L"))  return NT_LEFT;
  if (!strcmp(t, "R"))  return NT_RIGHT;
  if (!strcmp(t, "HL")) return NT_SHARP_L;
  if (!strcmp(t, "HR")) return NT_SHARP_R;
  if (!strcmp(t, "U"))  return NT_UTURN;
  if (!strcmp(t, "O"))  return NT_ROUND;
  if (!strcmp(t, "A"))  return NT_ARRIVE;
  return NT_STRAIGHT;
}

static volatile bool timeNew = false; static int timeV[6];
bool nav_take_time(int v[6]) { if (!timeNew) return false; memcpy(v, timeV, sizeof(timeV)); timeNew = false; return true; }

void nav_handle(const char* msg) {
  if (!msg || !*msg) return;
  if (msg[0] == 'T') {
    int v[6]; if (sscanf(msg, "T,%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6 && v[0] > 2024) {
      memcpy(timeV, v, sizeof(v)); timeNew = true; }
    return;
  }
  if (msg[0] == 'X') { ended = true; rpn = 0; return; }
  // the path comes in small pieces: "P..." starts it, "+..." continues, ';' ends it
  if (msg[0] == 'P' || msg[0] == '+') {
    if (msg[0] == 'P') plen = 0; else msg++;
    int l = strlen(msg);
    if (plen + l < (int)sizeof(pbuf)) { memcpy(pbuf + plen, msg, l); plen += l; pbuf[plen] = 0; }
    if (plen && pbuf[plen - 1] == ';') { parse_path(pbuf); plen = 0; }
    return;
  }
  if (msg[0] != 'N') return;
  char buf[48]; strncpy(buf, msg, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  char* save = NULL;
  strtok_r(buf, ",", &save);
  char* t = strtok_r(NULL, ",", &save);
  char* d = strtok_r(NULL, ",", &save);
  char* e = strtok_r(NULL, ",", &save);
  if (!t || !d) return;
  portENTER_CRITICAL(&mux);
  turn = parse_turn(t);
  metres = atoi(d);
  exitNo = e ? atoi(e) : 0;
  lastMsg = millis();
  ended = false;
  portEXIT_CRITICAL(&mux);
}

class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override { nav_handle(c->getValue().c_str()); }
};
class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { connected = true; }
  void onDisconnect(BLEServer* s) override { connected = false; s->getAdvertising()->start(); }
};

void nav_begin() {
  BLEDevice::init("Laboo");
  BLEDevice::setPower(ESP_PWR_LVL_N0);           // 0 dBm: the phone is in the same car; saves power
  BLEServer* s = BLEDevice::createServer();
  s->setCallbacks(new SrvCb());
  BLEService* svc = s->createService(NAV_SVC);
  BLECharacteristic* rx = svc->createCharacteristic(NAV_RX,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCb());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NAV_SVC);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

bool nav_connected() { return connected; }
bool nav_active() { return !ended && millis() - lastMsg < 15000; }
int nav_metres() { return metres; }
NavTurn nav_turn() { return turn; }

// ------------------------------------------------------------------ drawing
static inline uint16_t sw(uint16_t c) { return (c >> 8) | (c << 8); }
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
static inline uint16_t mix(uint16_t a, uint16_t b, int t) {        // normal colours, t 0..255
  int r = ((a >> 11) * (255 - t) + (b >> 11) * t) / 255;
  int g = (((a >> 5) & 63) * (255 - t) + ((b >> 5) & 63) * t) / 255;
  int bl = ((a & 31) * (255 - t) + (b & 31) * t) / 255;
  return (r << 11) | (g << 5) | bl;
}

struct V { float x, y; };
static float seg_d(float px, float py, V a, V b) {
  float vx = b.x - a.x, vy = b.y - a.y, wx = px - a.x, wy = py - a.y;
  float t = (wx * vx + wy * vy) / (vx * vx + vy * vy);
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  float dx = wx - vx * t, dy = wy - vy * t;
  return sqrtf(dx * dx + dy * dy);
}
static float tri_d(float px, float py, V a, V b, V c) {           // <0 inside
  auto edge = [&](V p, V q) { return ((q.x - p.x) * (py - p.y) - (q.y - p.y) * (px - p.x)) /
                                     sqrtf((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y)); };
  float e1 = edge(a, b), e2 = edge(b, c), e3 = edge(c, a);
  if (e1 < 0 && e2 < 0 && e3 < 0) return fmaxf(e1, fmaxf(e2, e3));
  if (e1 > 0 && e2 > 0 && e3 > 0) return -fminf(e1, fminf(e2, e3));
  return fminf(seg_d(px, py, a, b), fminf(seg_d(px, py, b, c), seg_d(px, py, c, a)));
}

// current arrow shape (built once per frame)
static V path[24]; static int np = 0;
static V head[3]; static bool hasHead = false;
static bool ring = false; static V ringC; static float ringR;
static const float HALF = 11;                                       // stem half width
static int ax0, ay0, ax1, ay1;                                      // bounding box

static void build_shape(NavTurn t) {
  np = 0; hasHead = true; ring = false;
  const V base = {180, 250}, mid = {180, 160};
  float ang = 0;                                                    // degrees, 0 = up, + = right
  switch (t) {
    case NT_SLIGHT_L: ang = -40; break;  case NT_SLIGHT_R: ang = 40; break;
    case NT_LEFT: ang = -90; break;      case NT_RIGHT: ang = 90; break;
    case NT_SHARP_L: ang = -135; break;  case NT_SHARP_R: ang = 135; break;
    default: break;
  }
  if (t == NT_UTURN) {
    path[np++] = {205, 250}; path[np++] = {205, 140};
    for (int i = 1; i <= 10; i++) {
      float a = 3.14159f * i / 10;
      path[np++] = {180 + 25 * cosf(a), 140 - 25 * sinf(a)};
    }
    path[np++] = {155, 185};
  } else if (t == NT_ROUND) {
    ring = true; ringC = {180, 165}; ringR = 38;
    path[np++] = base; path[np++] = {180, 165 + 38};
    // exit direction from the number of the exit (1 = right, 2 = straight, 3 = left...)
    float a = (exitNo <= 0 ? 2 : exitNo) * 1.5708f - 1.5708f;      // 0 = right
    float ex = cosf(a), ey = -sinf(a);
    V p0 = {ringC.x + ex * ringR, ringC.y + ey * ringR};
    path[np++] = {-1000, -1000};                                    // break
    path[np++] = p0; path[np++] = {p0.x + ex * 36, p0.y + ey * 36};
  } else if (t == NT_ARRIVE) {
    hasHead = false; ring = true; ringC = {180, 150}; ringR = 30;
    path[np++] = {180, 180}; path[np++] = {180, 250};
  } else if (t == NT_STRAIGHT) {
    path[np++] = base; path[np++] = {180, 95};
  } else {
    float r = ang * 0.0174533f;
    path[np++] = base; path[np++] = mid;
    path[np++] = {mid.x + sinf(r) * 70, mid.y - cosf(r) * 70};
  }
  if (hasHead && np >= 2) {
    V a = path[np - 2], b = path[np - 1];
    float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy); dx /= l; dy /= l;
    V tip = {b.x + dx * 30, b.y + dy * 30};
    head[0] = tip; head[1] = {b.x - dy * 30, b.y + dx * 30}; head[2] = {b.x + dy * 30, b.y - dx * 30};
  }
  ax0 = 60; ax1 = 300; ay0 = 40; ay1 = 275;
}

static float shape_d(float px, float py) {
  float d = 1e9f;
  for (int i = 0; i + 1 < np; i++) {
    if (path[i].x < -500 || path[i + 1].x < -500) continue;
    d = fminf(d, seg_d(px, py, path[i], path[i + 1]) - HALF);
  }
  if (hasHead) d = fminf(d, tri_d(px, py, head[0], head[1], head[2]));
  if (ring) d = fminf(d, fabsf(sqrtf((px - ringC.x) * (px - ringC.x) + (py - ringC.y) * (py - ringC.y)) - ringR) - 8);
  return d;
}

// 5x7 pixel letters for units, drawn with square dots
static const uint8_t L_K[7] = {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12};
static const uint8_t L_M[7] = {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15};
static void draw_letter(uint16_t* band, int y0, int h, const uint8_t* l, int x, int y, int s, uint16_t c) {
  for (int r = 0; r < 7; r++) for (int col = 0; col < 5; col++) {
    if (!(l[r] & (0x10 >> col))) continue;
    for (int yy = 0; yy < s; yy++) {
      int py = y + r * s + yy; if (py < y0 || py >= y0 + h) continue;
      for (int xx = 0; xx < s; xx++) band[(py - y0) * LCD_W + x + col * s + xx] = sw(c);
    }
  }
}
static int glyph(const GaugeFont& f, char ch) { for (int i = 0; f.chars[i]; i++) if (f.chars[i] == ch) return i; return -1; }
static int text_w(const GaugeFont& f, const char* s) { int w = 0; for (; *s; s++) { int i = glyph(f, *s); w += i < 0 ? 12 : f.g[i].w; } return w; }
static void draw_txt(uint16_t* band, int y0, int h, const GaugeFont& f, const char* s, int x, int y, uint16_t c) {
  for (; *s; s++) {
    int gi = glyph(f, *s);
    if (gi < 0) {                                                   // '.' -> a round dot
      for (int py = y + f.h - 6; py < y + f.h; py++) if (py >= y0 && py < y0 + h)
        for (int px = x + 3; px < x + 9; px++) band[(py - y0) * LCD_W + px] = sw(c);
      x += 12; continue;
    }
    int w = f.g[gi].w; const uint8_t* d = f.data + f.g[gi].off;
    for (int row = 0; row < f.h; row++) {
      int py = y + row; if (py < y0 || py >= y0 + h) continue;
      uint16_t* line = band + (py - y0) * LCD_W;
      for (int cx = 0; cx < w; cx++) {
        int idx = row * w + cx, a = (d[idx >> 1] >> ((idx & 1) ? 0 : 4)) & 15;
        if (a) line[x + cx] = sw(mix(sw(line[x + cx]), c, a * 17));
      }
    }
    x += w;
  }
}

// ------------------------------------------------------------------ map mode
#define CAR_X 180
#define CAR_Y 235
static float sdist(float px, float py) {                          // small turn arrow at the top
  const float s = 0.38f;
  return shape_d(180 + (px - 180) / s, 170 + (py - 78) / s) * s;
}
static void draw_map(uint16_t* b, int y0, int h, int n, const int16_t* xs, const int16_t* ys, uint8_t* cov) {
  const uint16_t line = rgb(255, 150, 30);
  memset(cov, 0, LCD_W * h);
  const float W = 7.5f, G = 9;                                     // half width, glow
  for (int i = 0; i + 1 < n; i++) {
    V a = {(float)CAR_X + xs[i], (float)CAR_Y - ys[i]}, c = {(float)CAR_X + xs[i + 1], (float)CAR_Y - ys[i + 1]};
    int x0 = (int)fminf(a.x, c.x) - W - G, x1 = (int)fmaxf(a.x, c.x) + W + G;
    int ya = (int)fminf(a.y, c.y) - W - G, yb = (int)fmaxf(a.y, c.y) + W + G;
    if (ya < y0) ya = y0; if (yb >= y0 + h) yb = y0 + h - 1; if (x0 < 0) x0 = 0; if (x1 >= LCD_W) x1 = LCD_W - 1;
    for (int y = ya; y <= yb; y++) for (int x = x0; x <= x1; x++) {
      float d = seg_d(x + 0.5f, y + 0.5f, a, c) - W;
      int v = d < -0.5f ? 255 : d < 0.5f ? (int)((0.5f - d) * 255) : d < G ? -(int)(70 * (1 - d / G) * (1 - d / G)) : 0;
      uint8_t& cv = cov[(y - y0) * LCD_W + x];
      int enc = v > 0 ? 128 + v / 2 : -v;                            // >=128: line, <128: glow strength
      if (enc > cv) cv = enc;
    }
  }
  for (int y = y0; y < y0 + h; y++) {
    uint16_t* l = b + (y - y0) * LCD_W; const uint8_t* cv = cov + (y - y0) * LCD_W;
    for (int x = 0; x < LCD_W; x++) {
      if (!cv[x]) continue;
      uint16_t c = sw(l[x]);
      if (cv[x] >= 128) c = mix(c, line, (cv[x] - 128) * 2);
      else c = mix(c, line, cv[x]);
      l[x] = sw(c);
    }
  }
  // car: white triangle pointing up with a dark edge
  V t0 = {CAR_X, CAR_Y - 16}, t1 = {CAR_X - 11, CAR_Y + 11}, t2 = {CAR_X + 11, CAR_Y + 11};
  for (int y = CAR_Y - 20; y <= CAR_Y + 15; y++) {
    if (y < y0 || y >= y0 + h) continue;
    uint16_t* l = b + (y - y0) * LCD_W;
    for (int x = CAR_X - 15; x <= CAR_X + 15; x++) {
      float d = tri_d(x + 0.5f, y + 0.5f, t0, t1, t2);
      if (d < 0.5f) l[x] = sw(mix(sw(l[x]), rgb(250, 250, 255), d < -0.5f ? 255 : (int)((0.5f - d) * 255)));
      else if (d < 3) l[x] = sw(mix(sw(l[x]), rgb(10, 10, 14), 200));
    }
  }
}

void nav_render_frame(float kmh, bool kmhValid) {
  NavTurn t; int m;
  portENTER_CRITICAL(&mux); t = turn; m = metres; portEXIT_CRITICAL(&mux);
  build_shape(t);
  bool near = m < 150 && t != NT_STRAIGHT;
  const uint16_t bgc = rgb(10, 10, 14), arrow = near ? rgb(255, 170, 40) : rgb(245, 245, 250),
                 glow = rgb(255, 140, 20), orange = rgb(255, 150, 30);

  char num[12]; bool km = m >= 1000;
  if (t == NT_ARRIVE && m < 30) strcpy(num, "0");
  else if (km) snprintf(num, sizeof(num), m < 10000 ? "%d.%d" : "%d", m / 1000, (m % 1000) / 100);
  else snprintf(num, sizeof(num), "%d", m < 100 ? (m / 5) * 5 : (m / 10) * 10);
  int nw = text_w(gfont_big, num), uw = km ? 2 * 5 * 3 + 4 : 5 * 3;
  int tx = 180 - (nw + 8 + uw) / 2;
  static uint8_t cov[LCD_W * BAND_H];
  int16_t xs[MAXP], ys[MAXP]; int n;
  portENTER_CRITICAL(&mux); n = (millis() - lastPath < 6000) ? rpn : 0; memcpy(xs, rpx, n * 2); memcpy(ys, rpy, n * 2); portEXIT_CRITICAL(&mux);
  bool map = n >= 2;

  for (int y0 = 0; y0 < LCD_H; y0 += BAND_H) {
    uint16_t* b = display_band_buffer();
    for (int y = y0; y < y0 + BAND_H; y++) {
      uint16_t* line = b + (y - y0) * LCD_W;
      for (int x = 0; x < LCD_W; x++) {
        float dx = x - 180, dy = y - 180, r = sqrtf(dx * dx + dy * dy);
        uint16_t c = r > 179 ? 0 : mix(rgb(24, 22, 30), bgc, (int)fminf(255, r * 1.4f));
        if (map) {
          if (r > 140 && r < 142) c = mix(c, rgb(60, 60, 72), 160);    // range ring
        } else if (x >= ax0 && x < ax1 && y >= ay0 && y < ay1) {
          float d = shape_d(x + 0.5f, y + 0.5f);
          if (d < 0.5f) c = mix(c, arrow, d < -0.5f ? 255 : (int)((0.5f - d) * 255));
          else if (d < 10) c = mix(c, glow, (int)(55 * (1 - d / 10) * (1 - d / 10)));
        }
        line[x] = sw(c);
      }
    }
    if (map) {
      draw_map(b, y0, BAND_H, n, xs, ys, cov);
      // small turn arrow in a dark disc at the top
      for (int y = 38; y < 120; y++) {
        if (y < y0 || y >= y0 + BAND_H) continue;
        uint16_t* l = b + (y - y0) * LCD_W;
        for (int x = 139; x < 222; x++) {
          float dd = sqrtf((x - 180.f) * (x - 180.f) + (y - 78.f) * (y - 78.f));
          if (dd > 41) continue;
          uint16_t c = dd > 39.5f ? rgb(255, 150, 30) : rgb(16, 16, 22);
          float d = sdist(x + 0.5f, y + 0.5f);
          if (d < 0.5f) c = mix(c, arrow, d < -0.5f ? 255 : (int)((0.5f - d) * 255));
          l[x] = sw(c);
        }
      }
    }
    // distance
    int ty = 288;
    draw_txt(b, y0, BAND_H, gfont_big, num, tx, ty, orange);
    int ux = tx + nw + 8, uy = ty + gfont_big.h - 21;
    if (km) { draw_letter(b, y0, BAND_H, L_K, ux, uy, 3, orange); draw_letter(b, y0, BAND_H, L_M, ux + 19, uy, 3, orange); }
    else draw_letter(b, y0, BAND_H, L_M, ux, uy, 3, orange);
    // speed from the car (small, top)
    if (kmhValid) {
      char s[8]; snprintf(s, sizeof(s), "%d", (int)(kmh + 0.5f));
      draw_txt(b, y0, BAND_H, gfont_small, s, 180 - text_w(gfont_small, s) / 2, 22, rgb(170, 170, 180));
    }
    // link dot: green = phone connected
    for (int y = 330; y < 336; y++) if (y >= y0 && y < y0 + BAND_H)
      for (int x = 177; x < 183; x++) b[(y - y0) * LCD_W + x] = sw(connected ? rgb(40, 220, 90) : rgb(120, 40, 40));
    display_push_band(y0, BAND_H);
  }
}
