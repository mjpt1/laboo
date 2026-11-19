#include "intro.h"
#include "display.h"
#include "sound.h"
#include <Preferences.h>
#include <math.h>

// Drawing works like face.cpp: each band is cleared, then shapes are drawn only where they are,
// row by row, with integer colour maths. (Per-pixel float maths over the whole screen ran at 3-6 fps.)

// ------------------------------------------------------------------ basics
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static inline float smooth(float a, float b, float v) { float t = clamp01((v - a) / (b - a)); return t * t * (3 - 2 * t); }

// the face's resting eyes (same numbers as face.cpp: Lumo sizes scaled to 360 px)
static const float K = 360.0f / 466.0f;
static const float EYE_RX = 45 * K, EYE_RY = 65 * K, EYE_DX = 85 * K, EYE_Y = 180 - 30 * K;
static const int OR = 255, OG = 165, OB = 0;                     // Lumo orange

static uint16_t* B;                                              // current band
static int BY0;                                                  // its first row
static const int BH = BAND_H;

// add colour (r,g,b) * a/255 to a panel pixel (big-endian RGB565), saturating
static inline void add_px(int x, int y, int r, int g, int b, int a) {
  if (a <= 0 || x < 0 || x >= LCD_W || y < BY0 || y >= BY0 + BH) return;
  if (a > 255) a = 255;
  uint16_t& p = B[(y - BY0) * LCD_W + x];
  uint16_t c = (p >> 8) | (p << 8);
  int R = ((c >> 11) << 3) + (r * a >> 8), G = (((c >> 5) & 63) << 2) + (g * a >> 8), Bl = ((c & 31) << 3) + (b * a >> 8);
  if (R > 255) R = 255; if (G > 255) G = 255; if (Bl > 255) Bl = 255;
  c = ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (Bl >> 3);
  p = (c >> 8) | (c << 8);
}
static inline uint16_t pack(int r, int g, int b) {
  if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
  uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  return (c >> 8) | (c << 8);
}

// filled ellipse with a soft edge, brightness k (0..1), drawn row by row
static void ellipse(float cx, float cy, float rx, float ry, int r, int g, int b, float k) {
  if (rx < 0.5f || ry < 0.5f || k <= 0) return;
  int ya = (int)floorf(cy - ry - 1), yb = (int)ceilf(cy + ry + 1);
  if (ya < BY0) ya = BY0; if (yb > BY0 + BH - 1) yb = BY0 + BH - 1;
  int a = (int)(255 * clamp01(k));
  uint16_t solid = pack(r * a >> 8, g * a >> 8, b * a >> 8);
  for (int y = ya; y <= yb; y++) {
    float dy = (y + 0.5f - cy) / ry;
    float cover = 1;                                            // soft top/bottom edge
    if (fabsf(dy) >= 1) { float over = (fabsf(dy) - 1) * ry; if (over > 1) continue; cover = 1 - over; dy = 1; }
    float half = rx * sqrtf(fmaxf(0, 1 - dy * dy));
    float xl = cx - half, xr = cx + half;
    int ia = (int)ceilf(xl), ib = (int)floorf(xr) - 1;
    uint16_t* l = B + (y - BY0) * LCD_W;
    if (cover >= 1) { for (int x = (ia < 0 ? 0 : ia); x <= ib && x < LCD_W; x++) l[x] = solid; }
    else for (int x = ia; x <= ib; x++) add_px(x, y, r, g, b, (int)(a * cover));
    add_px(ia - 1, y, r, g, b, (int)(a * cover * (ia - xl)));    // soft left / right edge
    add_px(ib + 1, y, r, g, b, (int)(a * cover * (xr - ib - 1)));
  }
}
// both eyes; open 0..1 squashes them (closed = thin line)
static void eyes(float open, float k = 1, float sc = 1, float spread = 1) {
  float ry = open < 0.06f ? 1.6f : EYE_RY * open * sc;
  ellipse(180 - EYE_DX * spread, EYE_Y, EYE_RX * sc, ry, OR, OG, OB, k);
  ellipse(180 + EYE_DX * spread, EYE_Y, EYE_RX * sc, ry, OR, OG, OB, k);
}
// soft round glow (falls to 0 at radius R)
static uint8_t sqrtT[1025];                                      // sqrt(i) * 8 for i < 1025 * 16 (see glow)
static void glow(float cx, float cy, float R, int r, int g, int b, float peak) {
  if (R < 1 || peak <= 0) return;
  int ya = (int)(cy - R), yb = (int)(cy + R) + 1, xa = (int)(cx - R), xb = (int)(cx + R) + 1;
  if (ya < BY0) ya = BY0; if (yb > BY0 + BH - 1) yb = BY0 + BH - 1;
  if (xa < 0) xa = 0; if (xb > LCD_W - 1) xb = LCD_W - 1;
  float inv = 1.0f / R; int pk = (int)(255 * peak);
  for (int y = ya; y <= yb; y++) {
    float dy = (y + 0.5f - cy) * inv, dy2 = dy * dy;
    for (int x = xa; x <= xb; x++) {
      float dx = (x + 0.5f - cx) * inv, d2 = dx * dx + dy2;
      if (d2 >= 1) continue;
      int f = 255 - (sqrtT[(int)(d2 * 1024)] * 255 >> 8);       // 1 - distance
      add_px(x, y, r, g, b, pk * f * f >> 16);
    }
  }
}

// one frame: clear each band, call draw(), push it
static float T;                                                  // seconds into the current scene
template <typename F> static void frame(F draw) {
  for (int y0 = 0; y0 < LCD_H; y0 += BAND_H) {
    B = display_band_buffer(); BY0 = y0;
    memset(B, 0, LCD_W * BAND_H * 2);
    draw();
    display_push_band(y0, BAND_H);
  }
}
static uint32_t nFrames = 0, msFrames = 0;                       // for the fps line in the boot log
template <typename F> static void scene(float secs, F draw) {
  uint32_t t0 = millis();
  for (;;) {
    T = (millis() - t0) / 1000.0f;
    if (T > secs) T = secs;
    uint32_t f0 = millis();
    draw();
    uint32_t spent = millis() - f0;
    nFrames++; msFrames += spent;
    if (spent < 25) delay(25 - spent);                           // steady 40 fps: even motion, no tearing bursts
    if (T >= secs) break;
  }
}
static uint8_t blTarget = 80;
static void bl(float k) { display_set_backlight((uint8_t)(blTarget * clamp01(k))); }
static uint32_t rng = 1234567;
static inline uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

// ------------------------------------------------------------------ 1: wake up
static bool wakeFromDark = true;
static void intro_wake() {
  scene(0.9f, [] {                                               // slits glow in the dark
    bl(wakeFromDark ? T / 0.6f : 1);
    float g = smooth(0, 0.9f, T);
    frame([&] { eyes(0, g); });
  });
  // half open, close, open, close, wide open
  static const float keys[][2] = {{0, 0}, {0.35f, 0.45f}, {0.55f, 0.05f}, {0.8f, 0.7f}, {0.95f, 0.05f}, {1.35f, 1.05f}, {1.6f, 1.0f}};
  scene(1.6f, [] {
    float o = 0;
    for (int i = 0; i < 6; i++) if (T >= keys[i][0] && T <= keys[i + 1][0]) {
      float k = smooth(keys[i][0], keys[i + 1][0], T); o = keys[i][1] + (keys[i + 1][1] - keys[i][1]) * k; }
    bl(1);
    frame([&] { eyes(o); });
  });
  sound_play(SND_HAPPY);
}

// ------------------------------------------------------------------ 2: old TV
static void intro_tv() {
  bl(1);
  scene(0.9f, [] {                                               // a dot, a line, then the screen opens
    float w = T < 0.3f ? 4 + 20 * smooth(0, 0.3f, T) : 24 + 156 * smooth(0.3f, 0.6f, T);
    float h = T < 0.6f ? 3 : 3 + 177 * smooth(0.6f, 0.9f, T);
    frame([&] {
      int ya = (int)(180 - h), yb = (int)(180 + h), xa = (int)(180 - w), xb = (int)(180 + w);
      for (int y = (ya > BY0 ? ya : BY0); y <= yb && y < BY0 + BH; y++) {
        uint16_t* l = B + (y - BY0) * LCD_W;
        for (int x = (xa < 0 ? 0 : xa); x <= xb && x < LCD_W; x++) {
          int n = h < 6 ? 255 : 90 + (rnd() & 165);
          l[x] = pack(n, n * 190 >> 8, n * 90 >> 8);
        }
      }
    });
  });
  scene(1.4f, [] {                                               // static fades, eyes come through it
    float st = 1 - smooth(0.1f, 1.2f, T), e = smooth(0.2f, 1.1f, T);
    int sk = (int)(255 * st), roll = (int)(T * 240);
    frame([&] {
      if (sk > 0) for (int y = BY0; y < BY0 + BH; y++) {
        uint16_t* l = B + (y - BY0) * LCD_W;
        int line = ((y + roll) % 9 == 0) ? 60 : 0;
        for (int x = 0; x < LCD_W; x++) {
          int n = ((rnd() & 255) * sk >> 8) + (line * sk >> 8);
          l[x] = pack(n * 200 >> 8, n * 140 >> 8, n * 60 >> 8);
        }
      }
      eyes(e > 0.98f ? 1 : 0.3f + 0.7f * e, e);
    });
  });
  sound_play(SND_HAPPY);
}

// ------------------------------------------------------------------ 3: boot ring + LABOO
static const uint8_t FONT[5][7] = {                              // L A B O O, 5x7
  {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
  {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
  {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}};
static const int TS = 7, TGAP = 9, TW = 5 * TS, TTOT = 5 * TW + 4 * TGAP, TX0 = 180 - TTOT / 2, TY0 = 180 - 7 * TS / 2;
static void laboo_text(float k) {                                // round dots, glowing white-orange
  if (k <= 0) return;
  int a = (int)(255 * k);
  for (int ch = 0; ch < 5; ch++) for (int row = 0; row < 7; row++) for (int col = 0; col < 5; col++) {
    if (!(FONT[ch][row] & (0x10 >> col))) continue;
    float cx = TX0 + ch * (TW + TGAP) + col * TS + TS / 2.0f, cy = TY0 + row * TS + TS / 2.0f;
    if (cy + TS < BY0 || cy - TS > BY0 + BH) continue;
    ellipse(cx, cy, TS * 0.5f, TS * 0.5f, 255 * a >> 8, 205 * a >> 8, 90 * a >> 8, 1);
  }
}
// ring tables (PSRAM, made once): angle (0..255 from the top, clockwise) and brightness, only
// for the band of rows/columns the ring covers
static uint8_t *tA = NULL, *tV = NULL;                           // angle, ring+glow brightness
static const int RIN = 146, ROUT = 190;                          // glow area of the ring
static bool ring_tables() {
  if (tA) return true;
  tA = (uint8_t*)ps_malloc(LCD_W * LCD_H); tV = (uint8_t*)ps_malloc(LCD_W * LCD_H);
  if (!tA || !tV) { tA = NULL; return false; }
  for (int y = 0; y < LCD_H; y++) for (int x = 0; x < LCD_W; x++) {
    int i = y * LCD_W + x;
    float dx = x + 0.5f - 180, dy = y + 0.5f - 180, r = sqrtf(dx * dx + dy * dy);
    float a = atan2f(dx, -dy); if (a < 0) a += 6.2832f;
    tA[i] = (uint8_t)(a / 6.2832f * 255.0f);
    float ring = (r > 160 && r < 176) ? clamp01(1 - fabsf(r - 168) / 6) : 0, gl = clamp01(1 - fabsf(r - 168) / 22) * 0.35f;
    tV[i] = (uint8_t)(255 * fminf(1, ring + gl));
  }
  return true;
}
static void ring(float p, float k) {                             // arc 0..1 from the top, brightness k
  if (k <= 0 || !tA) return;
  int pa = (int)(p * 256), kk = (int)(255 * k);
  for (int y = BY0; y < BY0 + BH; y++) {
    int dy = y - 180; if (dy < -ROUT || dy > ROUT) continue;
    int outer = (int)sqrtf((float)(ROUT * ROUT - dy * dy)), inner = abs(dy) < RIN ? (int)sqrtf((float)(RIN * RIN - dy * dy)) : -1;
    const uint8_t *A = tA + y * LCD_W, *V = tV + y * LCD_W;
    int spans[2][2] = {{180 - outer, 180 - inner}, {180 + inner, 180 + outer}};   // left / right part of the ring
    if (inner < 0) { spans[0][1] = 180 + outer; spans[1][0] = spans[1][1] = 0; }  // near top/bottom: one span
    for (int side = 0; side < 2; side++) {
      int xa = spans[side][0], xb = spans[side][1];
      for (int x = (xa < 0 ? 0 : xa); x < xb && x < LCD_W; x++) {
        if (A[x] >= pa || !V[x]) continue;
        int v = V[x] * kk >> 8, lag = pa - A[x];
        int head = (lag < 20 && p < 1) ? (20 - lag) * 12 : 0;    // bright leading edge
        add_px(x, y, OR + head, OG + head, head * 60 >> 8, v);
      }
    }
  }
}
static void ring_fill() {
  bl(1);
  ring_tables();
  scene(1.5f, [] { float p = smooth(0, 1.3f, T), t = smooth(0.7f, 1.4f, T); frame([&] { ring(p, 1); laboo_text(t); }); });
}
static void ring_fade_to_dark() {
  scene(0.6f, [] { float k = 1 - smooth(0, 0.6f, T); frame([&] { ring(1.01f, k); laboo_text(k); }); });
}
static void intro_ring() {
  ring_fill();
  scene(1.1f, [] {
    float rk = 1 - smooth(0.3f, 1.0f, T), tk = 1 - smooth(0, 0.4f, T), o = smooth(0.35f, 1.0f, T);
    frame([&] { ring(1.01f, rk); laboo_text(tk); if (o > 0) eyes(o); });
  });
  sound_play(SND_HAPPY);
}

// ------------------------------------------------------------------ 4: comet
static void intro_comet() {
  bl(1);
  const int N = 14;
  static float tx[N], ty[N];
  auto pos = [](float t, float& x, float& y) {                   // spiral in to the centre
    float k = smooth(0, 1.4f, t), a = -1.2f + k * 9.5f, r = 165 * (1 - k);
    x = 180 + r * sinf(a); y = 180 - r * cosf(a);
  };
  scene(1.4f, [&] {
    for (int i = 0; i < N; i++) pos(T - i * 0.025f, tx[i], ty[i]);
    frame([&] {
      for (int i = N - 1; i >= 0; i--) {
        float f = 1 - i / (float)N, rr = 9 - i * 0.5f;
        glow(tx[i], ty[i], rr + 14, 255, 190, 80, 0.55f * f);
        if (i == 0) ellipse(tx[0], ty[0], rr, rr, 255, 235, 180, 1);
      }
    });
  });
  sound_play(SND_SURPRISED);
  scene(1.0f, [] {                                               // split, fly to the eye places, grow
    float s = smooth(0, 0.55f, T), g = smooth(0.45f, 1.0f, T);
    float lx = 180 - EYE_DX * s, rx = 180 + EYE_DX * s, cy = 180 + (EYE_Y - 180) * s;
    float r = 9 + (EYE_RX - 9) * g, ry = 9 + (EYE_RY - 9) * g;
    frame([&] {
      if (g < 1) { glow(lx, cy, ry + 30, 255, 200, 90, 0.4f * (1 - g)); glow(rx, cy, ry + 30, 255, 200, 90, 0.4f * (1 - g)); }
      ellipse(lx, cy, r, ry, OR, OG, OB, 1); ellipse(rx, cy, r, ry, OR, OG, OB, 1);
    });
  });
}

// ------------------------------------------------------------------ 5: heartbeat
// heart drawn row by row: for each row, the curve gives where it starts and ends
static void heart(float cx, float cy, float s, int r, int g, int b, float k) {
  if (s < 1 || k <= 0) return;
  int ya = (int)(cy - 1.3f * s), yb = (int)(cy + 1.3f * s);
  if (ya < BY0) ya = BY0; if (yb > BY0 + BH - 1) yb = BY0 + BH - 1;
  int a = (int)(255 * k);
  for (int y = ya; y <= yb; y++) {
    float v = -(y + 0.5f - cy) / s + 0.25f;
    // find the half width by stepping in u (the curve (u^2+v^2-1)^3 - u^2 v^3 = 0)
    float lo = 0, hi = 1.3f, in0 = (v * v - 1) * (v * v - 1) * (v * v - 1);
    if (in0 > 0) continue;                                       // row outside the heart at the middle
    for (int it = 0; it < 12; it++) {
      float u = (lo + hi) / 2, q = u * u + v * v - 1, f = q * q * q - u * u * v * v * v;
      if (f <= 0) lo = u; else hi = u;
    }
    float half = lo * s, xl = cx - half, xr = cx + half;
    int ia = (int)ceilf(xl), ib = (int)floorf(xr) - 1;
    for (int x = ia; x <= ib; x++) add_px(x, y, r, g, b, a);
    add_px(ia - 1, y, r, g, b, (int)(a * (ia - xl)));
    add_px(ib + 1, y, r, g, b, (int)(a * (xr - ib - 1)));
  }
}
static void intro_heart() {
  bl(0);
  scene(1.8f, [] {
    bl(T / 0.4f);
    float bt = 0;                                                // two beats
    for (float t0 : {0.3f, 1.05f}) { float d = T - t0; if (d > 0 && d < 0.45f) bt = fmaxf(bt, sinf(d / 0.45f * 3.1416f)); }
    float s = 60 + 16 * bt;
    frame([&] { glow(180, 175, s * 2.2f, 255, 40, 80, 0.2f + 0.4f * bt); heart(180, 170, s, 255, 40, 70, 1); });
  });
  scene(1.1f, [] {                                               // splits into two, becomes the eyes
    float k = smooth(0, 0.8f, T), o = smooth(0.35f, 1.0f, T);
    float hs = 60 * (1 - k) + 1, cy = 170 + (EYE_Y - 170) * k;
    frame([&] {
      heart(180 - EYE_DX * k, cy, hs, 255, 40, 70, 1 - o);
      heart(180 + EYE_DX * k, cy, hs, 255, 40, 70, 1 - o);
      if (o > 0) eyes(o);
    });
  });
  sound_play(SND_LOVE);
}

// ------------------------------------------------------------------ public
void intro_play(int n, uint8_t backlight) {
  blTarget = backlight;
  if (!sqrtT[1024]) for (int i = 0; i <= 1024; i++) sqrtT[i] = (uint8_t)fminf(255, sqrtf(i / 1024.0f) * 256);
  display_set_backlight(0);
  frame([] {});                                                  // clear the panel's random RAM ("snow")
  switch (n) {
    case 1: intro_wake(); break;
    case 2: intro_tv(); break;
    case 3: intro_ring(); break;
    case 4: intro_comet(); break;
    case 5: intro_heart(); break;
    default: ring_fill(); ring_fade_to_dark(); wakeFromDark = false; intro_wake(); wakeFromDark = true; break;
  }
  display_set_backlight(backlight);
  if (nFrames) blog("[intro] %d: %lu frames, %.1f fps\n", n, nFrames, 1000.0f * nFrames / (msFrames ? msFrames : 1));
  nFrames = msFrames = 0;
}

int intro_saved() { Preferences p; p.begin("intro", true); int n = p.getInt("n", 6); p.end(); return n; }
void intro_save(int n) { Preferences p; p.begin("intro", false); p.putInt("n", n); p.end(); }
