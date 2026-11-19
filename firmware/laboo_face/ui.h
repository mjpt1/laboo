#pragma once
#include <Arduino.h>

// Shared band-drawing helpers + Persian text (Vazirmatn glyphs from ui_assets.h).
// Every screen draws 40-row bands; call ui_band() first so these know where to draw.
void ui_band(uint16_t* buf, int y0, int h);
uint16_t ui_rgb(float r, float g, float b);          // panel byte order
void ui_clear(uint16_t c);
void ui_span(int y, int x0, int x1, uint16_t c);
void ui_round_rect(float x, float y, float w, float h, float r, uint16_t c);
void ui_ellipse(float cx, float cy, float rx, float ry, uint16_t c);
void ui_set_text_color(float r, float g, float b);   // colour for the glyph calls below

enum UiFont { UI_BIG, UI_SMALL };
int  ui_number_width(UiFont f, int n, int minDigits = 1);
int  ui_number(UiFont f, int n, int x, int y, int minDigits = 1);   // returns right edge
int  ui_font_height(UiFont f);
void ui_text_centered(int textId, int cx, int y);    // TX_* from ui_assets.h
int  ui_text_width(int textId);
void ui_weekday(int mon0, int x, int y);             // draws right-aligned at x
int  ui_weekday_width(int mon0);
void ui_month(int jm, int x, int y);                 // jm 1..12, right-aligned at x
int  ui_month_width(int jm);

enum { TX_MORNING, TX_NIGHT, TX_TOO_EARLY, TX_MS, TX_BRAVO, TX_WAIT, TX_NOW, TX_COUNT };
