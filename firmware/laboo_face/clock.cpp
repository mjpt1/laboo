// Analog clock for Laboo's idle time: dial with Persian numerals, weekday + Jalali date,
// and two little Lumo eyes that keep blinking so it still feels alive.
// Text comes from ui.cpp (Vazirmatn glyphs). Drawn band by band.
#include "clock.h"
#include "display.h"
#include "ui.h"
#include <math.h>

static ClockTime now_t = {2026, 1, 1, 0, 0, 0, false};
static bool dim = false;
static float t = 0;
static float blink = 1; static bool blinking = false; static float blinkT = 0, nextBlink = 2.5f;
static float look = 0, lookT = 0, nextLook = 3;

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static uint32_t secStamp = 0;          // millis() when the seconds last changed: for the smooth sweep
void clock_set(const ClockTime& c) { if (c.s != now_t.s) secStamp = millis(); now_t = c; }
void clock_set_dim(bool d) { dim = d; }

// ---- what the two little eyes are up to (they get bored on a clock face)
enum { A_IDLE, A_FOLLOW, A_BORED, A_DATE, A_ROLL, A_NOD, A_WINK, A_CROSS, A_SQUINT, A_WIDE, A_PING, A_HIDE, A_PICK_END,
       A_MINUTE, A_HOUR };                      // the last two are triggered by the clock itself
static int act = A_IDLE; static float actT = 0, actLen = 4;
static float eyeX = 0, eyeY = 0, lidL = 1, lidR = 1, eyeS = 1, conv = 0, hop = 0;
static int lastMi = -1, lastH = -1;
static void startAct(int a) {
  static const float len[] = {5, 10, 7, 3.2f, 1.6f, 4.8f, 1.0f, 1.8f, 2.2f, 1.5f, 3.0f, 3.5f, 0, 2.5f, 3.2f};
  act = a; actT = 0; actLen = len[a] + (a == A_IDLE ? random(5000) / 1000.0f : 0);
}
void clock_update(float dt) {
  t += dt; actT += dt;
  if (!blinking && t > nextBlink) { blinking = true; blinkT = 0; }
  if (blinking) {
    blinkT += dt;
    float k = blinkT / 0.22f;
    blink = k < 0.5f ? 1 - k * 2 : (k - 0.5f) * 2;
    if (k >= 1) { blinking = false; blink = 1; nextBlink = t + 2.5f + random(3000) / 1000.0f; }
  }
  if (t > nextLook) { lookT = random(100) < 40 ? 0 : random(-100, 101) / 100.0f; nextLook = t + 2 + random(4000) / 1000.0f; }

  // the clock itself interrupts: a new minute gets a glance, a new hour a little party
  if (now_t.valid) {
    if (lastH >= 0 && now_t.h != lastH) startAct(A_HOUR);
    else if (lastMi >= 0 && now_t.mi != lastMi) startAct(A_MINUTE);
    lastMi = now_t.mi; lastH = now_t.h;
  }
  if (actT > actLen) startAct(act == A_IDLE ? 1 + random(A_PICK_END - 1) : A_IDLE);   // idle, trick, idle, trick...

  const float TWO_PI_F = 6.2831853f;
  float sec = now_t.s + fminf((millis() - secStamp) / 1000.0f, 0.999f);
  float tx = lookT, ty = 0, l = 1, r = 1, s = 1, cv = 0, hp = 0, k = actT / actLen;
  switch (act) {
    case A_FOLLOW: { float a = sec * TWO_PI_F / 60; tx = sinf(a); ty = -cosf(a); } break;           // chase the second hand
    case A_BORED:  l = r = k > 0.8f ? 0.15f : 0.45f; tx = sinf(actT * 0.8f) * 0.9f; ty = 0.3f; break; // half-shut, drifting, a sigh
    case A_DATE:   ty = 1; tx = 0.5f - fmodf(actT, 1.2f) / 1.2f; break;                              // read the date, right to left
    case A_ROLL:   { float a = k * TWO_PI_F * 2; tx = sinf(a); ty = -cosf(a); } break;               // eye roll
    case A_NOD:    if (actT < 4) { float q = actT / 4; l = r = 1 - 0.9f * q; ty = 0.6f * q; } else s = 1.3f; break;   // nods off, jolts awake
    case A_WINK:   r = actT < 0.5f ? 0.1f : 1; tx = 0; break;
    case A_CROSS:  cv = 1; tx = 0; ty = 0.2f; break;                                                // goes cross-eyed
    case A_SQUINT: l = r = 0.35f; tx = 0; break;                                                     // suspicious
    case A_WIDE:   s = 1.3f; ty = -0.8f; tx = 0; break;                                              // stares at the 12
    case A_PING:   tx = (fmodf(actT, 1.0f) < 0.5f) ? -1 : 1; break;                                  // tick... tock... left, right
    case A_HIDE:   hp = -26 * sinf(fminf(k, 1.0f) * 3.1416f); l = r = 0.7f; break;                    // ducks down and peeks back up
    case A_MINUTE: { float a = (now_t.mi + sec / 60) * TWO_PI_F / 60; tx = sinf(a); ty = -cosf(a); s = actT < 0.5f ? 1.25f : 1; } break;
    case A_HOUR:   hp = fabsf(sinf(actT * 8)) * 9; s = 1.15f; tx = 0; break;                          // happy hops
  }
  float e = clampf(dt * (act == A_FOLLOW || act == A_ROLL ? 18 : 9), 0, 1);
  eyeX += (tx - eyeX) * e; eyeY += (ty - eyeY) * e;
  lidL += (l - lidL) * e; lidR += (r - lidR) * e; eyeS += (s - eyeS) * e; conv += (cv - conv) * e; hop += (hp - hop) * e;
  look = eyeX;
}

// ---------------------------------------------------------------- Gregorian -> Jalali
void to_jalali(int gy, int gm, int gd, int& jy, int& jm, int& jd) {
  static const int gdm[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  int gy2 = (gm > 2) ? gy + 1 : gy;
  long days = 355666L + 365L * gy + (gy2 + 3) / 4 - (gy2 + 99) / 100 + (gy2 + 399) / 400 + gd + gdm[gm - 1];
  jy = -1595 + 33 * (int)(days / 12053); days %= 12053;
  jy += 4 * (int)(days / 1461); days %= 1461;
  if (days > 365) { jy += (int)((days - 1) / 365); days = (days - 1) % 365; }
  if (days < 186) { jm = 1 + (int)(days / 31); jd = 1 + (int)(days % 31); }
  else            { jm = 7 + (int)((days - 186) / 30); jd = 1 + (int)((days - 186) % 30); }
}
int weekday_mon0(int y, int m, int d) {          // Sakamoto, 0 = Sunday -> shift to Monday = 0
  static const int tt[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  int w = (y + y / 4 - y / 100 + y / 400 + tt[m - 1] + d) % 7;
  return (w + 6) % 7;
}

// ---------------------------------------------------------------- drawing
// Analog dial: 60 ticks, Persian ۱۲ ۳ ۶ ۹, smooth second hand, weekday + Jalali date in the lower half,
// and the two little eyes above the centre.
static uint16_t* CB; static int CY0;
static inline uint16_t blend565(uint16_t dst, uint16_t src, int a) {   // both big-endian, a 0..256
  uint16_t d = (dst >> 8) | (dst << 8), s = (src >> 8) | (src << 8);
  int r = (((s >> 11) & 31) * a + ((d >> 11) & 31) * (256 - a)) >> 8;
  int g = (((s >> 5) & 63) * a + ((d >> 5) & 63) * (256 - a)) >> 8;
  int b = ((s & 31) * a + (d & 31) * (256 - a)) >> 8;
  uint16_t o = (r << 11) | (g << 5) | b;
  return (o >> 8) | (o << 8);
}
// anti-aliased round-ended line of radius r (only the part inside the current band is touched)
static void capsule(float x0, float y0, float x1, float y1, float r, uint16_t c) {
  int ya = (int)floorf(fminf(y0, y1) - r - 1), yb = (int)ceilf(fmaxf(y0, y1) + r + 1);
  if (ya < CY0) ya = CY0; if (yb > CY0 + BAND_H - 1) yb = CY0 + BAND_H - 1;
  if (yb < ya) return;
  int xa = (int)floorf(fminf(x0, x1) - r - 1), xb = (int)ceilf(fmaxf(x0, x1) + r + 1);
  if (xa < 0) xa = 0; if (xb > LCD_W - 1) xb = LCD_W - 1;
  float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
  for (int y = ya; y <= yb; y++) {
    uint16_t* row = CB + (y - CY0) * LCD_W;
    // only the stretch of this row the line can reach (a long diagonal hand has a huge bounding box)
    int ra = xa, rb = xb;
    if (fabsf(dy) > 0.01f) {
      float ta = clampf((y - r - 1 - y0) / dy, 0, 1), tb = clampf((y + r + 2 - y0) / dy, 0, 1);
      float xs = x0 + dx * ta, xe = x0 + dx * tb;
      ra = (int)floorf(fminf(xs, xe) - r - 1); rb = (int)ceilf(fmaxf(xs, xe) + r + 1);
      if (ra < xa) ra = xa; if (rb > xb) rb = xb;
    }
    for (int x = ra; x <= rb; x++) {
      float px = x + 0.5f - x0, py = y + 0.5f - y0;
      float k = len2 > 0 ? clampf((px * dx + py * dy) / len2, 0, 1) : 0;
      float ex = px - k * dx, ey = py - k * dy;
      float a = r + 0.5f - sqrtf(ex * ex + ey * ey);
      if (a <= 0) continue;
      row[x] = a >= 1 ? c : blend565(row[x], c, (int)(a * 256));
    }
  }
}
static inline void polar(float ang, float rad, float& x, float& y) { x = 180 + sinf(ang) * rad; y = 180 - cosf(ang) * rad; }
static void hand(float ang, float len, float tail, float r, uint16_t c) {
  float x0, y0, x1, y1; polar(ang, -tail, x0, y0); polar(ang, len, x1, y1);
  capsule(x0, y0, x1, y1, r, c);
}
static void numeral(int n, int cx, int cy) {
  int w = ui_number_width(UI_SMALL, n), h = ui_font_height(UI_SMALL);
  ui_number(UI_SMALL, n, cx - w / 2, cy - h / 2);
}

static void drawBand() {
  ui_clear(ui_rgb(0, 0, 0));
  float br = dim ? 0.5f : 1.0f;
  const uint16_t org = ui_rgb(255 * br, 165 * br, 0), grey = ui_rgb(95 * br, 95 * br, 100 * br);
  const uint16_t white = ui_rgb(255 * br, 238 * br, 210 * br), red = ui_rgb(255 * br, 60 * br, 50 * br);
  const float TWO_PI_F = 6.2831853f;

  // ticks
  for (int i = 0; i < 60; i++) {
    float a = i * TWO_PI_F / 60, x0, y0, x1, y1;
    bool hour = i % 5 == 0, quarter = i % 15 == 0;
    polar(a, 174, x0, y0); polar(a, quarter ? 150 : hour ? 158 : 167, x1, y1);
    capsule(x0, y0, x1, y1, quarter ? 3.2f : hour ? 2.4f : 1.0f, hour ? org : grey);
  }
  // ۱۲ ۳ ۶ ۹
  ui_set_text_color(255 * br, 238 * br, 210 * br);
  numeral(12, 180, 54); numeral(3, 306, 180); numeral(6, 180, 306); numeral(9, 54, 180);

  // little eyes above the centre
  for (int i = 0; i < 2; i++) {
    float sg = i ? 1 : -1, lid = (i ? lidR : lidL) * (lidL > 0.6f ? blink : 1);
    float ex = 180 + eyeX * 7 + sg * (20 - conv * 6), ey = 112 + eyeY * 6 - hop;
    float rx = 8 * eyeS, ry = fmaxf(12 * eyeS * lid, 1.6f);
    if (ry >= rx) capsule(ex, ey - ry + rx, ex, ey + ry - rx, rx, org);      // tall capsule
    else          capsule(ex - rx + ry, ey, ex + rx - ry, ey, ry, org);      // lid nearly shut: a flat bar
  }

  if (!now_t.valid) {                               // clock not set yet: --:--
    const int dx[4] = {110, 148, 190, 228};
    for (int i = 0; i < 4; i++) ui_round_rect(dx[i], 228, 24, 9, 4.5f, org);
    capsule(180, 180, 180, 180, 7, org);
    return;
  }

  // date line, right to left: weekday, day, month
  int jy, jm, jd; to_jalali(now_t.y, now_t.mo, now_t.d, jy, jm, jd);
  int wd = weekday_mon0(now_t.y, now_t.mo, now_t.d);
  const int sp = 9, dy = 232 - ui_font_height(UI_SMALL) / 2;
  ui_set_text_color(255 * br, 165 * br, 0);
  int dw = ui_number_width(UI_SMALL, jd);
  int total = ui_weekday_width(wd) + sp + dw + sp + ui_month_width(jm);
  int xr = 180 + total / 2;
  ui_weekday(wd, xr, dy); xr -= ui_weekday_width(wd) + sp;
  ui_number(UI_SMALL, jd, xr - dw, dy); xr -= dw + sp;
  ui_month(jm, xr, dy);

  // hands (drawn over the date): hour, minute, sweeping second
  float sec = now_t.s + fminf((millis() - secStamp) / 1000.0f, 0.999f);
  float mn = now_t.mi + sec / 60, hr = (now_t.h % 12) + mn / 60;
  hand(hr * TWO_PI_F / 12, 86, 14, 7.5f, ui_rgb(0, 0, 0));     // thin black edge so hands stay readable
  hand(hr * TWO_PI_F / 12, 86, 14, 5.5f, org);
  hand(mn * TWO_PI_F / 60, 132, 16, 5.5f, ui_rgb(0, 0, 0));
  hand(mn * TWO_PI_F / 60, 132, 16, 3.6f, white);
  hand(sec * TWO_PI_F / 60, 150, 30, 1.4f, red);
  capsule(180, 180, 180, 180, 8, red);
  capsule(180, 180, 180, 180, 3, ui_rgb(0, 0, 0));
}

void clock_render_frame() {
  for (int y0 = 0; y0 < LCD_H; y0 += BAND_H) {
    CB = display_band_buffer(); CY0 = y0;
    ui_band(CB, y0, BAND_H);
    drawBand();
    display_push_band(y0, BAND_H);
  }
}
