// Animated robot face: tall eyes, eyebrows, open cartoon mouth.
// One colour (white). When angry, eyes + mouth turn red, it shakes, yells and
// "curses" with comic symbols (#!) above its head.
// Everything is drawn band by band (40 rows at a time) straight into DMA RAM.
#include "face.h"
#include "display.h"
#include <math.h>

// ------------------------------------------------------------------ params
struct FaceParams {
  float eyeW, eyeH, eyeR, gap, eyeY;
  float lidTop, lidAngle, happy, asym;
  float browY, browAngle, browRaiseL;
  float mouthCurve, mouthW, mouthY, mouthOpen;
  float must;          // moustache tips: + curl up, - droop
  float red;           // 0 = white, 1 = red (angry)
};

static const FaceParams PRESETS[EMO_COUNT] = {
  // eyeW eyeH eyeR gap eyeY | lidT  lidA  happy asym | brY  brA   brL | mCurv mW  mY   mOpen | must | red
  {  66, 104, 24, 84, 162,   0.00, 0.00, 0.00, 0.0,   24,  0.00, 0,    0.30, 46, 268, 0.00,   0.3,  0 },  // neutral
  {  68,  98, 26, 84, 160,   0.00, 0.00, 0.40, 0.0,   30, -0.15, 0,    1.00, 70, 266, 0.50,   0.7,  0 },  // happy
  {  62,  92, 24, 84, 170,   0.15,-0.35, 0.00, 0.0,   18, -0.55, 0,   -0.80, 40, 272, 0.00,  -0.6,  0 },  // sad
  {  66,  96, 22, 82, 164,   0.25, 0.50, 0.00, 0.0,    6,  0.70, 0,   -0.50, 74, 266, 0.55,  -0.4,  1 },  // angry
  {  74, 120, 34, 86, 158,   0.00, 0.00, 0.00, 0.0,   42,  0.00, 0,    0.00, 40, 270, 0.50,   0.2,  0 },  // surprised
  {  66, 104, 24, 84, 162,   0.00, 0.00, 0.00, 0.0,   28, -0.10, 0,    0.80, 62, 266, 0.35,   0.5,  0 },  // love (heart eyes)
  {  66, 104, 24, 84, 162,   0.08, 0.00, 0.00, 0.3,   18,  0.05, 20,  -0.25, 36, 268, 0.00,   0.0,  0 },  // confused
  {  66, 104, 24, 84, 168,   0.55, 0.00, 0.00, 0.0,   14, -0.10, 0,    0.05, 30, 268, 0.00,   0.0,  0 },  // sleepy
};

static FaceParams cur = PRESETS[EMO_NEUTRAL];
static Emotion   emotion = EMO_NEUTRAL;
static uint32_t  emotionUntil = 0;
static FaceState state = ST_IDLE;

// dynamic values
static float lookX = 0, lookY = 0, lookTX = 0, lookTY = 0;
static bool  lookOverride = false;
static float blinkOpen = 1.0f;
static uint32_t nextBlink = 2000, blinkStart = 0; static bool blinking = false, doubleBlink = false;
static uint32_t nextSaccade = 1500;
static float talk = 0, talkTarget = 0; static uint32_t talkStamp = 0;   // lip sync 0..1
static float micLevel = 0, micSlow = 0, attention = 0, ring = 0;
static float squash = 0;
static uint32_t lastPoke = 0;
static float t = 0;
static float drawW = 66, drawH = 104;
static float shakeX = 0, shakeY = 0;
static float heart = 0;             // 0..1 blend to heart eyes

static inline float lerpf(float a, float b, float k) { return a + (b - a) * k; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint16_t rgb(float r, float g, float b) {
  int R = (int)clampf(r, 0, 255), G = (int)clampf(g, 0, 255), B = (int)clampf(b, 0, 255);
  uint16_t c = ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3);
  return (c >> 8) | (c << 8);      // panel wants big-endian
}

// ------------------------------------------------------------------ public API
void face_init() { lastPoke = millis(); }

void face_set_emotion(Emotion e, uint32_t ms) {
  emotion = e;
  emotionUntil = ms ? millis() + ms : 0;
}
void face_set_state(FaceState s) {
  if (s != ST_SLEEPING) face_poke();
  state = s;
  if (s == ST_SLEEPING) display_set_backlight(15); else display_set_backlight(85);
}
FaceState face_get_state() { return state; }
void face_set_mouth(int level) { talkTarget = clampf(level / 100.0f, 0, 1); talkStamp = millis(); }
void face_set_mic_level(float l) { micLevel = clampf(l, 0, 1); }
void face_look(float x, float y) { lookOverride = true; lookTX = clampf(x, -1, 1) * 45; lookTY = clampf(y, -1, 1) * 35; }
void face_look_release() { lookOverride = false; }
void face_blink() { blinking = true; blinkStart = millis(); }
void face_poke() {
  lastPoke = millis();
  if (state == ST_SLEEPING) { state = ST_IDLE; display_set_backlight(85); squash = 1.0f; }
}

Emotion emotion_from_name(const char* s, bool* ok) {
  static const char* names[EMO_COUNT] = {"neutral","happy","sad","angry","surprised","love","confused","sleepy"};
  for (int i = 0; i < EMO_COUNT; i++) if (!strcasecmp(s, names[i])) { *ok = true; return (Emotion)i; }
  *ok = false; return EMO_NEUTRAL;
}
FaceState state_from_name(const char* s, bool* ok) {
  static const char* names[] = {"idle","listening","thinking","speaking","sleeping"};
  for (int i = 0; i < 5; i++) if (!strcasecmp(s, names[i])) { *ok = true; return (FaceState)i; }
  *ok = false; return ST_IDLE;
}

// ------------------------------------------------------------------ animation
void face_update(float dt) {
  uint32_t now = millis();
  t += dt;
  if (emotionUntil && now > emotionUntil) { emotion = EMO_NEUTRAL; emotionUntil = 0; }

  // ---- react to sound in the room (only when not busy talking with the laptop)
  float prevSlow = micSlow;
  micSlow = lerpf(micSlow, micLevel, 1.0f - expf(-dt * 3.0f));
  if (state == ST_SLEEPING && micLevel > 0.45f) face_poke();                  // loud voice wakes it up
  if (state == ST_IDLE) {
    if (micLevel > 0.55f && prevSlow < 0.08f && emotion == EMO_NEUTRAL) {    // sudden sound: startled
      face_set_emotion(EMO_SURPRISED, 700); face_blink(); lastPoke = now;
    }
    if (micSlow > 0.10f) lastPoke = now;                                    // someone is talking: stay awake
  }
  float attT = (state == ST_IDLE && micSlow > 0.10f) ? 1.0f : 0.0f;
  attention = lerpf(attention, attT, 1.0f - expf(-dt * 4.0f));

  uint32_t idle = now - lastPoke;
  if (state == ST_IDLE && idle > 180000) face_set_state(ST_SLEEPING);

  FaceParams tg = PRESETS[emotion];
  if (state == ST_IDLE && idle > 90000 && emotion == EMO_NEUTRAL) tg = PRESETS[EMO_SLEEPY];
  if (state == ST_LISTENING) { tg.eyeW += 4; tg.eyeH += 8; tg.browY += 10; }
  if (state == ST_THINKING)  { tg.browRaiseL = 14; tg.mouthOpen = 0; tg.mouthCurve = -0.1f; tg.mouthW = 30; }
  if (state == ST_SLEEPING)  { tg = PRESETS[EMO_SLEEPY]; tg.lidTop = 0; tg.eyeY = 172; tg.browY = 12; }
  tg.browY += attention * 10;          // eyebrows go up when it hears you
  tg.eyeH  += attention * 6;

  float k = 1.0f - expf(-dt * 10.0f);
  float* a = (float*)&cur; const float* b = (const float*)&tg;
  for (size_t i = 0; i < sizeof(FaceParams) / sizeof(float); i++) a[i] = lerpf(a[i], b[i], k);
  heart = lerpf(heart, emotion == EMO_LOVE && state != ST_SLEEPING ? 1.0f : 0.0f, k);

  // ---- blinking
  if (!blinking && now > nextBlink && state != ST_SLEEPING) { blinking = true; blinkStart = now; }
  if (blinking) {
    float p = (now - blinkStart) / 160.0f;
    blinkOpen = p < 0.5f ? 1 - p * 2 : (p - 0.5f) * 2;
    if (p >= 1) {
      blinking = false; blinkOpen = 1;
      if (!doubleBlink && random(100) < 20) { doubleBlink = true; nextBlink = now + 120; }
      else { doubleBlink = false; nextBlink = now + random(2000, 6000); }
    }
  }
  float sleepOpen = (state == ST_SLEEPING) ? 0.06f + 0.02f * sinf(t * 1.5f) : 1.0f;

  // ---- where to look
  if (!lookOverride) {
    if (state == ST_THINKING) { lookTX = 32; lookTY = -28; }
    else if (state == ST_SLEEPING) { lookTX = 0; lookTY = 10; }
    else if (attention > 0.5f) { lookTX = 0; lookTY = -6; }
    else if (state == ST_LISTENING || state == ST_SPEAKING) {
      if (now > nextSaccade) { lookTX = random(-12, 13); lookTY = random(-8, 9); nextSaccade = now + random(900, 2500); }
    } else if (now > nextSaccade) {
      if (random(100) < 35) { lookTX = 0; lookTY = 0; }
      else { lookTX = random(-40, 41); lookTY = random(-28, 29); }
      nextSaccade = now + random(1200, 4000);
    }
  }
  float lk = 1.0f - expf(-dt * 18.0f);
  lookX = lerpf(lookX, lookTX, lk);
  lookY = lerpf(lookY, lookTY, lk);

  // ---- mouth / ring / squash / shake
  if (now - talkStamp > 300 || state != ST_SPEAKING) talkTarget = 0;
  talk = lerpf(talk, talkTarget, 1.0f - expf(-dt * 25.0f));
  float ringT = (state == ST_LISTENING) ? 0.35f + micLevel * 0.65f : 0;
  ring = lerpf(ring, ringT, 1.0f - expf(-dt * 15.0f));
  squash = lerpf(squash, 0, 1.0f - expf(-dt * 8.0f));

  if (emotion == EMO_ANGRY && state != ST_SLEEPING) {
    float amp = 1.5f + 4.0f * talk;
    shakeX = random(-100, 101) / 100.0f * amp; shakeY = random(-100, 101) / 100.0f * amp;
  } else { shakeX = shakeY = 0; }

  drawH = cur.eyeH * blinkOpen * sleepOpen * (1.0f - 0.25f * squash);
  drawW = cur.eyeW * (1.0f + 0.15f * squash);
  if (state == ST_SLEEPING || blinking) drawH = fmaxf(drawH, 6);
}

// ------------------------------------------------------------------ primitives (band clipped)
static uint16_t* B;
static int BY0, BH;

static inline void span(int y, int x0, int x1, uint16_t c) {
  if (y < BY0 || y >= BY0 + BH) return;
  if (x0 < 0) x0 = 0; if (x1 > LCD_W - 1) x1 = LCD_W - 1;
  if (x1 < x0) return;
  uint16_t* p = B + (y - BY0) * LCD_W + x0;
  for (int x = x0; x <= x1; x++) *p++ = c;
}
static inline bool rowRange(float top, float bot, int& y0, int& y1) {
  y0 = (int)top > BY0 ? (int)top : BY0;
  y1 = (int)bot < BY0 + BH - 1 ? (int)bot : BY0 + BH - 1;
  return y1 >= y0;
}

static void fillRoundRect(float cx, float cy, float w, float h, float r, uint16_t c) {
  if (w < 1 || h < 1) return;
  r = fminf(r, fminf(w, h) * 0.5f);
  float L = cx - w / 2, R = cx + w / 2, T = cy - h / 2, Bo = cy + h / 2;
  int y0, y1; if (!rowRange(T, Bo, y0, y1)) return;
  for (int y = y0; y <= y1; y++) {
    float yc = y + 0.5f, inset = 0;
    if (yc < T + r) { float d = T + r - yc; inset = r - sqrtf(fmaxf(r * r - d * d, 0)); }
    else if (yc > Bo - r) { float d = yc - (Bo - r); inset = r - sqrtf(fmaxf(r * r - d * d, 0)); }
    span(y, (int)(L + inset + 0.5f), (int)(R - inset - 0.5f), c);
  }
}

static void fillEllipse(float cx, float cy, float rx, float ry, uint16_t c) {
  if (rx < 0.5f || ry < 0.5f) return;
  int y0, y1; if (!rowRange(cy - ry, cy + ry, y0, y1)) return;
  for (int y = y0; y <= y1; y++) {
    float d = (y + 0.5f - cy) / ry; if (d * d > 1) continue;
    float dx = rx * sqrtf(1 - d * d);
    span(y, (int)(cx - dx + 0.5f), (int)(cx + dx - 0.5f), c);
  }
}

static void fillRing(float cx, float cy, float ro, float ri, uint16_t c) {
  int y0, y1; if (!rowRange(cy - ro, cy + ro, y0, y1)) return;
  for (int y = y0; y <= y1; y++) {
    float dy = y + 0.5f - cy;
    if (dy * dy > ro * ro) continue;
    float xo = sqrtf(ro * ro - dy * dy);
    if (dy * dy >= ri * ri) { span(y, (int)(cx - xo), (int)(cx + xo), c); continue; }
    float xi = sqrtf(ri * ri - dy * dy);
    span(y, (int)(cx - xo), (int)(cx - xi), c);
    span(y, (int)(cx + xi), (int)(cx + xo), c);
  }
}

// line with round ends
static void capsule(float x0, float y0, float x1, float y1, float r, uint16_t c) {
  if (fminf(y0, y1) - r > BY0 + BH || fmaxf(y0, y1) + r < BY0) return;
  float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
  int n = (int)(len / 1.5f) + 1;
  for (int i = 0; i <= n; i++) {
    float u = (float)i / n;
    fillEllipse(x0 + (x1 - x0) * u, y0 + (y1 - y0) * u, r, r, c);
  }
}

// thick parabola: smile (curve>0) or frown (curve<0)
static void curveLine(float cx, float cy, float w, float curve, float thick, uint16_t c) {
  int x0 = (int)(cx - w / 2), x1 = (int)(cx + w / 2);
  for (int x = x0; x <= x1; x++) {
    float tt = (x - cx) / (w / 2);
    float y = cy + curve * 14 * (1 - tt * tt) - curve * 7;
    float th = thick * (1 - 0.35f * tt * tt);
    for (int yy = (int)(y - th / 2); yy <= (int)(y + th / 2); yy++) span(yy, x, x, c);
  }
}

// ------------------------------------------------------------------ face parts
static void drawEye(float cx, float cy, float w, float h, float r, float inward, uint16_t col, uint16_t bg) {
  fillRoundRect(cx, cy, w, h, r, col);
  float T = cy - h / 2, Bo = cy + h / 2;
  if (cur.lidTop > 0.01f || fabsf(cur.lidAngle) > 0.01f) {
    int y0, y1;
    if (rowRange(T, Bo, y0, y1)) for (int y = y0; y <= y1; y++) {
      float base = T + cur.lidTop * h;
      float s = cur.lidAngle * inward;
      int xl = (int)(cx - w / 2), xr = (int)(cx + w / 2);
      if (fabsf(s) < 0.001f) { if (y < base) span(y, xl, xr, bg); continue; }
      float xcut = clampf(cx + (y - base) / s, xl, xr + 1);
      if (s > 0) span(y, (int)xcut, xr, bg);
      else       span(y, xl, (int)xcut, bg);
    }
  }
  if (cur.happy > 0.01f) {
    float ry = h * 0.6f, rx = w * 0.85f;
    fillEllipse(cx, Bo + ry - cur.happy * h * 0.75f, rx, ry, bg);
  }
}

static void drawHeart(float cx, float cy, float R, uint16_t c) {
  fillEllipse(cx - R * 0.62f, cy - R * 0.35f, R * 0.72f, R * 0.72f, c);
  fillEllipse(cx + R * 0.62f, cy - R * 0.35f, R * 0.72f, R * 0.72f, c);
  float top = cy - R * 0.25f, tip = cy + R * 1.35f;
  int y0, y1; if (!rowRange(top, tip, y0, y1)) return;
  for (int y = y0; y <= y1; y++) {
    float hw = 1.33f * R * (1 - (y - top) / (tip - top));
    span(y, (int)(cx - hw), (int)(cx + hw), c);
  }
}

static void drawBrow(float cx, float y, float inward, float raise, uint16_t c) {
  float half = 25, d = cur.browAngle * 14;
  float yi = y - raise + d, yo = y - raise - d;          // inner / outer end
  float xi = cx + inward * half, xo = cx - inward * half;
  capsule(xo, yo, xi, yi, 6, c);
}

// Handlebar moustache: two plump halves, tips curl up (or droop). Built from
// overlapping circles along a curve, so the outline stays smooth.
static void drawMoustache(float cx, float cy, float w, uint16_t c) {
  if (cy - 20 > BY0 + BH || cy + 20 < BY0) return;
  float hw = w / 2;
  for (int side = -1; side <= 1; side += 2) {
    for (float u = 0.06f; u <= 1.0f; u += 0.025f) {
      float curl = fmaxf(0, (u - 0.62f) / 0.38f);
      float x = cx + side * u * hw;
      float y = cy + 5 * sinf(u * 3.1416f) - cur.must * 13 * curl * curl;
      float r = 8.5f * (1 - 0.62f * u) + 1.8f;
      fillEllipse(x, y, r, r * 0.9f, c);
    }
  }
}

// "D" shaped open mouth (flat top, round bottom) with teeth and tongue
static float dHalfWidth(float y, float top, float W, float H) {
  if (y < top || y > top + H) return -1;
  float t = (y - top) / H;
  float hw = W / 2 * sqrtf(fmaxf(0, 1 - t * t));
  float rc = 6, dy = y - top;
  if (dy < rc) hw -= rc - sqrtf(fmaxf(0, rc * rc - (rc - dy) * (rc - dy)));
  return hw;
}
static void drawOpenMouth(float cx, float cy, float W, float H, uint16_t col, uint16_t dark, uint16_t tongue) {
  float th = 5, top = cy - H * 0.4f;
  int y0, y1; if (!rowRange(top, top + H, y0, y1)) return;
  float iTop = top + th, iW = W - 2 * th, iH = H - 2 * th;
  float teeth = 5 + H * 0.12f;
  for (int y = y0; y <= y1; y++) {
    float yc = y + 0.5f;
    float ho = dHalfWidth(yc, top, W, H);
    if (ho < 0) continue;
    span(y, (int)(cx - ho), (int)(cx + ho), col);
    float hi = dHalfWidth(yc, iTop, iW, iH);
    if (hi <= 0 || iH < 6) continue;
    uint16_t fill = (yc < iTop + teeth && iH > 14) ? col : dark;
    span(y, (int)(cx - hi), (int)(cx + hi), fill);
    if (iH > 18) {   // tongue: an ellipse sitting at the bottom, clipped to the mouth
      float tcy = iTop + iH * 0.95f, try_ = iH * 0.38f, trx = iW * 0.32f;
      float d = (yc - tcy) / try_;
      if (d * d < 1) {
        float tw = fminf(trx * sqrtf(1 - d * d), hi - 1);
        if (tw > 0) span(y, (int)(cx - tw), (int)(cx + tw), tongue);
      }
    }
  }
}

// Wide shouting mouth with top and bottom jagged teeth
static void drawYell(float cx, float cy, float W, float H, uint16_t col, uint16_t dark) {
  fillRoundRect(cx, cy, W, H, H * 0.35f, col);
  float iw = W - 10, ih = H - 10;
  if (ih < 6) return;
  fillRoundRect(cx, cy, iw, ih, ih * 0.3f, dark);
  float L = cx - iw / 2, T = cy - ih / 2, Bo = cy + ih / 2;
  for (int x = (int)L + 3; x <= (int)(L + iw) - 3; x++) {
    float ph = fmodf((x - L) / 11.0f, 1.0f);
    float tooth = 4 + ih * 0.14f + (1 - fabsf(ph * 2 - 1)) * 5;   // triangle wave = pointy teeth
    for (int y = (int)T; y <= (int)(T + tooth); y++) span(y, x, x, col);
    for (int y = (int)(Bo - tooth); y <= (int)Bo; y++) span(y, x, x, col);
  }
}

// comic "cursing" symbols: # ! lightning and an anger mark
static void drawSymbol(int kind, float x, float y, uint16_t c) {
  switch (kind) {
    case 0:  // #
      capsule(x - 3, y - 11, x - 6, y + 11, 2.5f, c); capsule(x + 6, y - 11, x + 3, y + 11, 2.5f, c);
      capsule(x - 11, y - 4, x + 11, y - 4, 2.5f, c); capsule(x - 12, y + 4, x + 10, y + 4, 2.5f, c);
      break;
    case 1:  // !
      capsule(x, y - 12, x, y + 3, 3.5f, c); fillEllipse(x, y + 11, 3.5f, 3.5f, c);
      break;
    case 2:  // lightning
      capsule(x - 7, y - 12, x + 3, y - 1, 2.5f, c); capsule(x + 3, y - 1, x - 4, y + 2, 2.5f, c);
      capsule(x - 4, y + 2, x + 7, y + 13, 2.5f, c);
      break;
    default: // anger mark
      for (int q = 0; q < 4; q++) {
        float a = q * 1.5708f + 0.785f, ca = cosf(a), sa = sinf(a);
        capsule(x + ca * 5 - sa * 5, y + sa * 5 + ca * 5, x + ca * 12, y + sa * 12, 2.5f, c);
        capsule(x + ca * 5 + sa * 5, y + sa * 5 - ca * 5, x + ca * 12, y + sa * 12, 2.5f, c);
      }
  }
}

// ------------------------------------------------------------------ one band
static void drawBand() {
  const uint16_t bg = rgb(0, 0, 0);
  for (int i = 0; i < LCD_W * BH; i++) B[i] = bg;

  float br = (state == ST_SLEEPING) ? 0.55f : 1.0f;
  const float W_ = 245;                                        // white
  uint16_t white = rgb(W_ * br, W_ * br, (W_ - 5) * br);
  float rr = lerpf(W_, 255, cur.red), gg = lerpf(W_, 35, cur.red), bb = lerpf(W_ - 5, 30, cur.red);
  uint16_t col = rgb(rr * br, gg * br, bb * br);               // eyes + mouth (red when angry)
  uint16_t dark = rgb(28 + 30 * cur.red, 14, 18);
  uint16_t tongue = rgb(rr * 0.45f, gg * 0.40f, bb * 0.42f);

  if (ring > 0.03f) {
    float th = 4 + ring * 14;
    fillRing(180, 180, 179, 179 - th, rgb(W_ * ring, W_ * ring, W_ * ring));
  }

  float breathe = (state == ST_SLEEPING || state == ST_IDLE) ? sinf(t * 1.6f) * 1.5f : 0;
  float ex = lookX + shakeX, ey = lookY + breathe + shakeY;
  float pl = 1.0f - lookX * 0.0025f, pr = 1.0f + lookX * 0.0025f;
  float al = 1.0f - cur.asym * 1.2f, ar = 1.0f + cur.asym * 0.15f;
  float lx = 180 - cur.gap + ex, rx = 180 + cur.gap + ex, yy = cur.eyeY + ey;

  // eyes
  if (heart > 0.5f) {
    float R = 30 + 3 * sinf(t * 7);
    drawHeart(lx, yy, R, col); drawHeart(rx, yy, R, col);
  } else {
    drawEye(lx, yy, drawW * pl, drawH * pl * al, cur.eyeR, +1, col, bg);
    drawEye(rx, yy, drawW * pr, drawH * pr * ar, cur.eyeR, -1, col, bg);
  }

  // eyebrows (follow the eye's full height, not the blink)
  float browBase = yy - cur.eyeH / 2 - 10 - cur.browY * 0.5f;
  drawBrow(lx, browBase, +1, cur.browRaiseL, white);
  drawBrow(rx, browBase, -1, 0, white);

  // mouth
  float mx = 180 + ex * 0.6f, my = cur.mouthY + ey * 0.6f;
  float open = fmaxf(cur.mouthOpen, 0);
  if (state == ST_SPEAKING) open = fmaxf(open * 0.6f, 0.12f + 0.88f * talk);
  float mustY;
  if (state == ST_THINKING) {
    for (int i = 0; i < 3; i++) {
      float r = 5 + 3.5f * fmaxf(0, sinf(t * 6 - i * 0.9f));
      fillEllipse(mx - 24 + i * 24, my + 4, r, r, col);
    }
    mustY = my - 26;
  } else if (state == ST_SLEEPING) {
    curveLine(mx, my, 22, 0, 5, col);
    mustY = my - 20;
  } else if (emotion == EMO_ANGRY && open > 0.1f) {
    float H = 22 + 44 * open;
    drawYell(mx, my + H * 0.15f, cur.mouthW + 12 * talk, H, col, dark);
    mustY = my - H * 0.35f - 12;
  } else if (emotion == EMO_SURPRISED && state != ST_SPEAKING) {
    float r = 13 + 8 * open;
    fillEllipse(mx, my + 4, r, r * 1.2f, col);
    fillEllipse(mx, my + 4, r - 5, r * 1.2f - 5, dark);
    mustY = my - r * 1.2f - 10;
  } else if (open > 0.08f) {
    float H = 12 + 50 * open;
    drawOpenMouth(mx, my, cur.mouthW + 8 * talk, H, col, dark, tongue);
    mustY = my - H * 0.4f - 8;
  } else {
    curveLine(mx, my, cur.mouthW, cur.mouthCurve, 7, col);
    mustY = my - 25;
  }
  (void)mustY;                                    // (moustache removed)

  // cursing symbols above the head when angry
  if (emotion == EMO_ANGRY && state != ST_SLEEPING && cur.red > 0.5f) {
    int phase = (int)(t * 4);
    for (int i = 0; i < 3; i++) {
      float sx = 132 + i * 48 + sinf(t * 9 + i) * 3, sy = 52 + (i == 1 ? -8 : 0) + cosf(t * 8 + i) * 3;
      drawSymbol((phase + i) % 4, sx, sy, col);
    }
  }
}

void face_render_frame() {
  for (int y0 = 0; y0 < LCD_H; y0 += BAND_H) {
    B = display_band_buffer(); BY0 = y0; BH = BAND_H;
    drawBand();
    display_push_band(y0, BAND_H);
  }
}
