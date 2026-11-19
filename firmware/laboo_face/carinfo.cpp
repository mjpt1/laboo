// Car info page (second page in car mode): battery, fuel, consumption, trip distance and time,
// top speed, and the stored fault codes. Icons instead of words; numbers in Persian digits.
#include "carinfo.h"
#include "obd.h"
#include "display.h"
#include "ui.h"
#include <math.h>

static uint16_t ORG, DIM, RED, BLK;

static void dash(int cx, int y) { ui_round_rect(cx - 12, y + ui_font_height(UI_SMALL) / 2 - 3, 24, 6, 3, DIM); }
static void valInt(int n, int cx, int y, bool ok) {
  if (!ok) { dash(cx, y); return; }
  ui_number(UI_SMALL, n, cx - ui_number_width(UI_SMALL, n) / 2, y);
}
static void val1(float v, int cx, int y, bool ok) {             // one decimal
  if (!ok) { dash(cx, y); return; }
  int n = (int)(v * 10 + 0.5f), a = n / 10, b = n % 10, h = ui_font_height(UI_SMALL);
  int x = cx - (ui_number_width(UI_SMALL, a) + 9 + ui_number_width(UI_SMALL, b)) / 2;
  x = ui_number(UI_SMALL, a, x, y);
  ui_round_rect(x + 2, y + h - 7, 5, 5, 2.5f, ORG);
  ui_number(UI_SMALL, b, x + 9, y);
}
static void valTime(uint32_t sec, int cx, int y) {              // h:mm
  int hh = sec / 3600, mm = (sec / 60) % 60, h = ui_font_height(UI_SMALL);
  int x = cx - (ui_number_width(UI_SMALL, hh) + 11 + ui_number_width(UI_SMALL, mm, 2)) / 2;
  x = ui_number(UI_SMALL, hh, x, y);
  ui_round_rect(x + 3, y + h / 2 - 7, 5, 5, 2.5f, ORG);
  ui_round_rect(x + 3, y + h / 2 + 3, 5, 5, 2.5f, ORG);
  ui_number(UI_SMALL, mm, x + 11, y, 2);
}
static void tri(int cx, int y, int h, bool up, uint16_t c) {     // small triangle
  for (int i = 0; i < h; i++) { int w = up ? i : h - 1 - i; ui_span(y + i, cx - w, cx + w, c); }
}

// ---- icons (24 px tall boxes, centred on cx, top at y)
static void icBattery(int cx, int y, float volts) {
  ui_round_rect(cx - 15, y + 3, 28, 16, 3, ORG); ui_round_rect(cx + 13, y + 8, 4, 6, 1, ORG);
  ui_round_rect(cx - 12, y + 6, 22, 10, 2, BLK);
  float k = (volts - 11.5f) / 3.0f; k = k < 0 ? 0 : (k > 1 ? 1 : k);
  if (volts > 0) ui_round_rect(cx - 11, y + 7, 20 * k + 1, 8, 1, volts < 12.0f ? RED : ORG);
}
static void icFuel(int cx, int y) {
  ui_round_rect(cx - 10, y + 1, 15, 21, 2, ORG); ui_round_rect(cx - 7, y + 4, 9, 6, 1, BLK);
  ui_round_rect(cx + 5, y + 7, 6, 3, 1, ORG); ui_round_rect(cx + 9, y + 7, 3, 13, 1, ORG);
}
static void icDrop(int cx, int y) { tri(cx, y + 1, 11, true, ORG); ui_ellipse(cx, y + 14, 9, 8, ORG); }
static void icRoad(int cx, int y) { ui_round_rect(cx - 14, y + 9, 20, 5, 2, ORG); for (int i = 0; i < 9; i++) ui_span(y + 3 + i, cx + 5, cx + 5 + i, ORG), ui_span(y + 20 - i, cx + 5, cx + 5 + i, ORG); }
static void icClock(int cx, int y) {
  ui_ellipse(cx, y + 12, 11, 11, ORG); ui_ellipse(cx, y + 12, 8, 8, BLK);
  ui_round_rect(cx - 1, y + 6, 3, 7, 1, ORG); ui_round_rect(cx - 1, y + 11, 7, 3, 1, ORG);
}
static void icTop(int cx, int y) { tri(cx, y + 2, 10, true, ORG); tri(cx, y + 11, 10, true, ORG); }
static void icWarn(int cx, int y, uint16_t c) {
  ui_ellipse(cx, y + 12, 12, 12, c); ui_round_rect(cx - 2, y + 4, 4, 10, 2, BLK); ui_round_rect(cx - 2, y + 16, 4, 4, 2, BLK);
}

// ---- 5x7 letters for fault codes ("P0301")
static const char F_CH[] = "0123456789ABCDEFPU";
static const uint8_t F_PX[][7] = {
  {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
  {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
  {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
  {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
  {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
  {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}};
static int codeW(const char* s) { return (int)strlen(s) * 12 - 2; }
static void code(const char* s, int x, int y, uint16_t c) {    // 2x scale
  for (; *s; s++, x += 12) {
    const char* p = strchr(F_CH, *s); if (!p) continue;
    const uint8_t* g = F_PX[p - F_CH];
    for (int r = 0; r < 7; r++) for (int b = 0; b < 5; b++)
      if (g[r] & (0x10 >> b)) { ui_span(y + r * 2, x + b * 2, x + b * 2 + 1, c); ui_span(y + r * 2 + 1, x + b * 2, x + b * 2 + 1, c); }
  }
}

static void drawBand(const ObdData& o) {
  ORG = ui_rgb(255, 165, 0); DIM = ui_rgb(110, 75, 0); RED = ui_rgb(255, 60, 50); BLK = ui_rgb(0, 0, 0);
  ui_clear(BLK);
  ui_set_text_color(255, 165, 0);
  const int VY = 27;                                            // value sits this far under its icon

  icBattery(180, 30, o.volts);              val1(o.volts, 180, 30 + VY, o.volts > 0);
  icFuel(96, 104);                          valInt((int)(o.fuelPct + 0.5f), 96, 104 + VY, o.fuelPct >= 0);
  icTop(180, 104);                          valInt((int)o.maxKmh, 180, 104 + VY, o.maxKmh > 0);
  float l100 = o.tripKm > 0.3f && o.tripL > 0 ? o.tripL / o.tripKm * 100 : -1;      // trip average, L/100 km
  icDrop(264, 104);                         val1(l100, 264, 104 + VY, l100 >= 0 && l100 < 99);
  icRoad(118, 186);                         val1(o.tripKm, 118, 186 + VY, true);
  icClock(242, 186);                        valTime(o.tripSec, 242, 186 + VY);

  // fault codes: green-ish quiet dot when clean, red mark + the codes when not
  if (o.dtcCount > 0) {
    icWarn(180, 262, RED);
    int n = 0; while (n < 2 && o.dtc[n][0]) n++;
    int w = n ? codeW(o.dtc[0]) * n + 14 * (n - 1) : 0, x = 180 - w / 2;
    for (int i = 0; i < n; i++) { code(o.dtc[i], x, 296, RED); x += codeW(o.dtc[i]) + 14; }
  } else {
    icWarn(180, 270, o.dtcCount == 0 ? ui_rgb(40, 200, 90) : DIM);   // 0 = checked and clean, -1 = not read yet
  }
}

void carinfo_render_frame(bool demo) {
  static ObdData fake;
  if (demo) {                                                   // made-up values to look at the layout
    fake = obd_data();
    fake.volts = 13.8f; fake.fuelPct = 62; fake.maxKmh = 117; fake.tripKm = 23.4f; fake.tripL = 1.7f; fake.tripSec = 41 * 60;
    fake.dtcCount = 2; strcpy(fake.dtc[0], "P0301"); strcpy(fake.dtc[1], "P0420");
  }
  const ObdData& o = demo ? fake : obd_data();
  for (int y0 = 0; y0 < LCD_H; y0 += BAND_H) {
    ui_band(display_band_buffer(), y0, BAND_H);
    drawBand(o);
    display_push_band(y0, BAND_H);
  }
}
