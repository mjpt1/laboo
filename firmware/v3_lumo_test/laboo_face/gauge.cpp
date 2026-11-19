// Sport car gauge (carbon / glowing style): the speed arc lights up green -> yellow -> orange up to the
// current speed, glowing orange needle, big orange speed in the hex panel, coolant temp (+ bar) and clock.
// The dial and the fully-lit arc are pre-drawn pictures (gauge_assets.h); only moving parts are drawn here.
#include "gauge.h"
#include "display.h"
#include "gauge_assets.h"
#include <math.h>

static uint16_t* bg = NULL;              // dial, arc off           (panel byte order)
static uint16_t* lit = NULL;             // arc fully lit, 0 elsewhere
static uint16_t* amap = NULL;            // per pixel: position along the arc 0..27000 (0xFFFF = not on the arc)
static float spd = 0, spdShown = 0, spdVel = 0;
static float rpm = 0;
static int temp = -1;
static int hh = -1, mm = -1;
static bool live = false, demo = false;
static float demoT = 0;
static uint32_t lastData = 0;

static inline uint16_t sw(uint16_t c) { return (c >> 8) | (c << 8); }
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
static inline uint16_t blend(uint16_t panelPx, uint16_t c, int a /*0..255*/) {
  uint16_t p = sw(panelPx);
  int r0 = (p >> 11) & 31, g0 = (p >> 5) & 63, b0 = p & 31;
  int r1 = (c >> 11) & 31, g1 = (c >> 5) & 63, b1 = c & 31;
  int r = r0 + ((r1 - r0) * a) / 255, g = g0 + ((g1 - g0) * a) / 255, b = b0 + ((b1 - b0) * a) / 255;
  return sw((uint16_t)((r << 11) | (g << 5) | b));
}
static inline uint16_t add(uint16_t panelPx, uint16_t c, int a) {      // additive glow
  uint16_t p = sw(panelPx);
  int r = ((p >> 11) & 31) + (((c >> 11) & 31) * a) / 255;
  int g = ((p >> 5) & 63) + (((c >> 5) & 63) * a) / 255;
  int b = (p & 31) + ((c & 31) * a) / 255;
  if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
  return sw((uint16_t)((r << 11) | (g << 5) | b));
}

static void unrle(const uint8_t* src, uint32_t len, uint16_t* dst) {
  uint8_t* px = (uint8_t*)dst;
  uint32_t p = 0, total = LCD_W * LCD_H;
  for (uint32_t i = 0; i + 2 < len + 1 && p < total; i += 3) {
    uint32_t n = src[i] + 1;
    uint8_t hi = src[i + 1], lo = src[i + 2];
    while (n-- && p < total) { px[2 * p] = hi; px[2 * p + 1] = lo; p++; }
  }
}

bool gauge_init() {
  size_t n = LCD_W * LCD_H * 2;
  bg = (uint16_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  lit = (uint16_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  amap = (uint16_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!bg || !lit || !amap) return false;
  unrle(gauge_bg_rle, gauge_bg_len, bg);
  unrle(gauge_lit_rle, gauge_lit_len, lit);
  const float r0 = G_GLOW_R0, r1 = G_GLOW_R1;
  for (int y = 0; y < LCD_H; y++)
    for (int x = 0; x < LCD_W; x++) {
      uint16_t v = 0xFFFF;
      float dx = x + 0.5f - G_CX, dy = y + 0.5f - G_CY, d = sqrtf(dx * dx + dy * dy);
      if (d >= r0 && d <= r1 && lit[y * LCD_W + x]) {
        float a = atan2f(dy, dx) * 57.29578f;
        float t = fmodf(a - G_A0 + 720.0f, 360.0f);
        if (t > G_SWEEP) t = (t > G_SWEEP + (360 - G_SWEEP) / 2) ? 0 : G_SWEEP;   // glow just past the ends
        v = (uint16_t)(t * 100);
      }
      amap[y * LCD_W + x] = v;
    }
  return true;
}

void gauge_set(float kmh, float r, int coolant) {
  spd = kmh < 0 ? 0 : (kmh > G_VMAX ? G_VMAX : kmh);
  rpm = r;
  temp = coolant;
  live = true;
  demo = false;
  lastData = millis();
}
void gauge_set_time(int h, int m) { hh = h; mm = m; }
void gauge_demo(bool on) { demo = on; if (on) demoT = 0; }
bool gauge_is_demo() { return demo; }

static void demo_step(float dt) {
  demoT += dt;
  float t = fmodf(demoT, 26.0f);
  float top = 175.0f * (1 - expf(-11 / 4.5f));
  float v;
  if (t < 11) v = 175.0f * (1 - expf(-t / 4.5f));
  else if (t < 16) v = top - (t - 11) * 4;
  else v = fmaxf(0, (top - 20) * (1 - (t - 16) / 9.0f));
  spd = v;
  rpm = 850 + v * 30;
  temp = 88 + (int)(v / 25);
}

void gauge_update(float dt) {
  if (demo) demo_step(dt);
  else if (live && millis() - lastData > 3000) live = false;
  if (!demo && !live) { spd = 0; rpm = 0; }
  float k = 60.0f, c = 2.0f * sqrtf(k);
  spdVel += (k * (spd - spdShown) - c * spdVel) * dt;
  spdShown += spdVel * dt;
  if (spdShown < 0) { spdShown = 0; spdVel = 0; }
}

// ---------------------------------------------------------------- text
static int glyph_index(const GaugeFont& f, char ch) {
  for (int i = 0; f.chars[i]; i++) if (f.chars[i] == ch) return i;
  return -1;
}
static int text_width(const GaugeFont& f, const char* s) {
  int w = 0;
  for (; *s; s++) { int i = glyph_index(f, *s); w += i < 0 ? f.h / 3 : f.g[i].w; }
  return w;
}
static void draw_text(uint16_t* band, int by0, int bh, const GaugeFont& f, const char* s, int cx, int y,
                      uint16_t col, bool glow) {
  int x = cx - text_width(f, s) / 2;
  for (; *s; s++) {
    int gi = glyph_index(f, *s);
    if (gi < 0) { x += f.h / 3; continue; }
    int w = f.g[gi].w;
    const uint8_t* d = f.data + f.g[gi].off;
    for (int row = 0; row < f.h; row++) {
      int py = y + row;
      if (py < by0 || py >= by0 + bh) continue;
      uint16_t* line = band + (py - by0) * LCD_W;
      for (int colx = 0; colx < w; colx++) {
        int idx = row * w + colx;
        int a = (d[idx >> 1] >> ((idx & 1) ? 0 : 4)) & 15;
        int px = x + colx;
        if (!a || px < 1 || px >= LCD_W - 1) continue;
        line[px] = blend(line[px], col, a * 17);
        if (glow) { line[px - 1] = add(line[px - 1], col, a * 3); line[px + 1] = add(line[px + 1], col, a * 3); }
      }
    }
    x += w;
  }
}

// ---------------------------------------------------------------- one band
static void draw_band(uint16_t* band, int y0, int h) {
  memcpy(band, bg + y0 * LCD_W, LCD_W * h * 2);
  const float cx = G_CX, cy = G_CY;

  // speed arc: lit up to the needle
  uint16_t lim = (uint16_t)(G_SWEEP * spdShown / G_VMAX * 100);
  if (spdShown > 0.5f) {
    int ylo = y0, yhi = y0 + h;
    if (yhi > G_CY + G_GLOW_R1 + 1) yhi = G_CY + G_GLOW_R1 + 1;
    for (int y = ylo; y < yhi; y++) {
      const uint16_t* am = amap + y * LCD_W;
      const uint16_t* li = lit + y * LCD_W;
      uint16_t* line = band + (y - y0) * LCD_W;
      for (int x = 0; x < LCD_W; x++)
        if (am[x] <= lim) line[x] = li[x];
    }
  }

  // numbers
  const uint16_t orange = rgb(255, 150, 30);
  char b[16];
  if (demo || live) snprintf(b, sizeof(b), "%d", (int)(spdShown + 0.5f)); else strcpy(b, "--");
  draw_text(band, y0, h, gfont_big, b, 180, G_SPEED_Y, orange, true);
  if (temp >= 0) snprintf(b, sizeof(b), "%dd", temp); else strcpy(b, "--d");
  draw_text(band, y0, h, gfont_small, b, G_TEMP_X, G_SMALL_Y, temp >= 105 ? rgb(255, 50, 40) : orange, false);
  if (hh >= 0) snprintf(b, sizeof(b), "%02d:%02d", hh, mm); else strcpy(b, "--:--");
  draw_text(band, y0, h, gfont_small, b, G_TIME_X, G_SMALL_Y, orange, false);

  // temperature bar (40..120 C)
  if (temp >= 0 && G_BAR_Y + 2 >= y0 && G_BAR_Y - 2 < y0 + h) {
    float f = (temp - 40) / 80.0f; f = f < 0 ? 0 : (f > 1 ? 1 : f);
    int xe = G_BAR_X0 + (int)((G_BAR_X1 - G_BAR_X0) * f);
    uint16_t c = temp >= 105 ? rgb(255, 50, 40) : orange;
    for (int y = G_BAR_Y - 2; y <= G_BAR_Y + 1; y++) {
      if (y < y0 || y >= y0 + h) continue;
      uint16_t* line = band + (y - y0) * LCD_W;
      for (int x = G_BAR_X0 + 1; x < xe; x++) line[x] = sw(c);
    }
  }

  // glowing needle
  float ang = (G_A0 + G_SWEEP * spdShown / G_VMAX) * 0.0174533f;
  float ux = cosf(ang), uy = sinf(ang);
  const float len = G_NEEDLE_LEN, start = 10;
  float ax = cx + ux * start, ay = cy + uy * start, bx = cx + ux * len, by = cy + uy * len;
  int minx = (int)fminf(ax, bx) - 9, maxx = (int)fmaxf(ax, bx) + 9;
  int miny = (int)fminf(ay, by) - 9, maxy = (int)fmaxf(ay, by) + 9;
  const uint16_t core = rgb(255, 225, 150), body = rgb(255, 140, 20);
  for (int py = (miny > y0 ? miny : y0); py <= maxy && py < y0 + h; py++) {
    uint16_t* line = band + (py - y0) * LCD_W;
    for (int px = (minx < 0 ? 0 : minx); px <= maxx && px < LCD_W; px++) {
      float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
      float u = dx * ux + dy * uy, dp = fabsf(-dx * uy + dy * ux);
      if (u < start - 3 || u > len + 3) continue;
      float t = (u - start) / (len - start);
      float halfw = 3.2f - 1.8f * (t < 0 ? 0 : (t > 1 ? 1 : t));
      if (dp < halfw + 6) {                                  // soft glow
        float g = 1.0f - (dp - halfw) / 6.0f;
        if (dp > halfw && g > 0 && u > start && u < len) line[px] = add(line[px], body, (int)(g * g * 120));
      }
      float cov = halfw - dp + 0.5f;
      if (cov > 0 && u >= start && u <= len) {
        line[px] = blend(line[px], body, cov >= 1 ? 255 : (int)(cov * 255));
        float cc = halfw * 0.45f - dp + 0.5f;                 // hot centre line
        if (cc > 0) line[px] = blend(line[px], core, cc >= 1 ? 255 : (int)(cc * 255));
      }
    }
  }
  // hub: dark centre, glowing orange ring
  for (int py = (int)cy - 22; py <= (int)cy + 22; py++) {
    if (py < y0 || py >= y0 + h) continue;
    uint16_t* line = band + (py - y0) * LCD_W;
    for (int px = (int)cx - 22; px <= (int)cx + 22; px++) {
      float d = sqrtf((px + 0.5f - cx) * (px + 0.5f - cx) + (py + 0.5f - cy) * (py + 0.5f - cy));
      if (d <= 11) line[px] = sw(rgb(14, 14, 16));
      else if (d <= 14) line[px] = sw(orange);
      else if (d <= 21) line[px] = add(line[px], orange, (int)(90 * (21 - d) / 7));
    }
  }
}

void gauge_render_frame() {
  if (!bg) return;
  for (int y0 = 0; y0 < LCD_H; y0 += BAND_H) {
    uint16_t* b = display_band_buffer();
    draw_band(b, y0, BAND_H);
    display_push_band(y0, BAND_H);
  }
}
