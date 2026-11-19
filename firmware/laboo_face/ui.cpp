#include "ui.h"
#include "display.h"
#include <math.h>
#include <stdio.h>
#include "ui_assets.h"

static uint16_t* B; static int BY0, BH;
static uint16_t pal[16];

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void ui_band(uint16_t* buf, int y0, int h) { B = buf; BY0 = y0; BH = h; }
uint16_t ui_rgb(float r, float g, float b) {
  int R = (int)clampf(r, 0, 255), G = (int)clampf(g, 0, 255), Bc = (int)clampf(b, 0, 255);
  uint16_t c = ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (Bc >> 3);
  return (c >> 8) | (c << 8);
}
void ui_clear(uint16_t c) { for (int i = 0; i < LCD_W * BH; i++) B[i] = c; }
void ui_span(int y, int x0, int x1, uint16_t c) {
  if (y < BY0 || y >= BY0 + BH) return;
  if (x0 < 0) x0 = 0; if (x1 > LCD_W - 1) x1 = LCD_W - 1;
  if (x1 < x0) return;
  uint16_t* p = B + (y - BY0) * LCD_W + x0;
  for (int x = x0; x <= x1; x++) *p++ = c;
}
void ui_round_rect(float x, float y, float w, float h, float r, uint16_t c) {
  if (w < 1 || h < 1) return;
  r = fminf(r, fminf(w, h) * 0.5f);
  int y0 = (int)floorf(y), y1 = (int)ceilf(y + h) - 1;
  if (y0 < BY0) y0 = BY0; if (y1 > BY0 + BH - 1) y1 = BY0 + BH - 1;
  for (int yy = y0; yy <= y1; yy++) {
    float yc = yy + 0.5f, inset = 0;
    if (yc < y + r) { float d = y + r - yc; inset = r - sqrtf(fmaxf(r * r - d * d, 0)); }
    else if (yc > y + h - r) { float d = yc - (y + h - r); inset = r - sqrtf(fmaxf(r * r - d * d, 0)); }
    ui_span(yy, (int)lroundf(x + inset), (int)lroundf(x + w - inset) - 1, c);
  }
}
void ui_ellipse(float cx, float cy, float rx, float ry, uint16_t c) {
  if (rx < 0.5f || ry < 0.5f) return;
  int y0 = (int)floorf(cy - ry), y1 = (int)ceilf(cy + ry);
  if (y0 < BY0) y0 = BY0; if (y1 > BY0 + BH - 1) y1 = BY0 + BH - 1;
  for (int y = y0; y <= y1; y++) {
    float d = (y + 0.5f - cy) / ry; if (d * d > 1) continue;
    float dx = rx * sqrtf(1 - d * d);
    ui_span(y, (int)(cx - dx + 0.5f), (int)(cx + dx - 0.5f), c);
  }
}
void ui_set_text_color(float r, float g, float b) {
  for (int i = 0; i < 16; i++) pal[i] = ui_rgb(r * i / 15.0f, g * i / 15.0f, b * i / 15.0f);
}

static void glyph(const Glyph& g, int x0, int y0) {
  if (y0 >= BY0 + BH || y0 + g.h <= BY0) return;
  int rb = (g.w + 1) / 2;
  int ys = y0 > BY0 ? y0 : BY0, ye = (y0 + g.h < BY0 + BH) ? y0 + g.h : BY0 + BH;
  for (int y = ys; y < ye; y++) {
    const uint8_t* src = g.a + (y - y0) * rb;
    uint16_t* row = B + (y - BY0) * LCD_W;
    for (int px = 0; px < g.w; px++) {
      int x = x0 + px; if (x < 0 || x >= LCD_W) continue;
      uint8_t v = src[px >> 1]; v = (px & 1) ? (v & 15) : (v >> 4);
      if (v) row[x] = pal[v];
    }
  }
}
static const Glyph* fontSet(UiFont f) { return f == UI_BIG ? BIG : SMALL; }
int ui_font_height(UiFont f) { return fontSet(f)[0].h; }
int ui_number_width(UiFont f, int n, int minDigits) {
  char buf[12]; snprintf(buf, sizeof(buf), minDigits == 2 ? "%02d" : "%d", n);
  const Glyph* s = fontSet(f); int gap = f == UI_BIG ? 4 : 2, w = 0;
  for (char* p = buf; *p; p++) w += s[*p - '0'].w + gap;
  return w - gap;
}
int ui_number(UiFont f, int n, int x, int y, int minDigits) {   // Persian digits, left to right
  char buf[12]; snprintf(buf, sizeof(buf), minDigits == 2 ? "%02d" : "%d", n);
  const Glyph* s = fontSet(f); int gap = f == UI_BIG ? 4 : 2;
  for (char* p = buf; *p; p++) { glyph(s[*p - '0'], x, y); x += s[*p - '0'].w + gap; }
  return x - gap;
}
int  ui_text_width(int id) { return TEXT[id].w; }
void ui_text_centered(int id, int cx, int y) { glyph(TEXT[id], cx - TEXT[id].w / 2, y); }
int  ui_weekday_width(int d) { return WEEK[d].w; }
void ui_weekday(int d, int x, int y) { glyph(WEEK[d], x - WEEK[d].w, y); }
int  ui_month_width(int m) { return MONTH[m - 1].w; }
void ui_month(int m, int x, int y) { glyph(MONTH[m - 1], x - MONTH[m - 1].w, y); }
