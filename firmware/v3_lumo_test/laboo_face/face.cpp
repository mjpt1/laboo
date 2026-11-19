// LABOO face, Lumo style (v3 test).
// Eyes ported from Lumo (github.com/Aymo/Lumo, src/eye_engine.cpp + emotion.cpp, GPL-3.0):
// pupil-less capsule eyes; expressions come only from the capsule shape, black
// eyelids from top and bottom, and a black wedge "eyebrow" cut into the top.
// All Lumo sizes were made for a 466 px screen and are scaled here to 360 px.
//
// Laboo extras kept so the laptop program still works: the USB command API in
// face.h, listening ring, a small mouth only while speaking, thinking dots,
// sleeping, and the two emotions Lumo doesn't have (sad, confused).
// Drawn band by band (40 rows at a time) straight into DMA RAM, like before.
#include "face.h"
#include "display.h"
#include "heart_lumo.h"
#include <math.h>

static const float K = 360.0f / 466.0f;          // Lumo screen -> Laboo screen

// ------------------------------------------------------------------ eye parameters
struct EyeParams {
  float rx, ry;        // capsule half width / half height
  float sp;            // distance between the two eye centres
  float vo;            // vertical offset (negative = up)
  float ot, ob;        // eyelid openness top / bottom: 1 = open, 0 = closed
  float brow;          // + angry wedge (points to the nose), - sad wedge (outer corner down)
  float asym;          // confused: left eye squints, right eye opens wider
  bool  heart;
  bool  red;
};

static EyeParams base() {
  EyeParams p;
  p.rx = 45 * K; p.ry = 65 * K; p.sp = 170 * K; p.vo = 0;
  p.ot = 1; p.ob = 1; p.brow = 0; p.asym = 0; p.heart = false; p.red = false;
  return p;
}

static EyeParams preset(Emotion e) {
  EyeParams p = base();
  switch (e) {
    case EMO_NEUTRAL: break;
    case EMO_HAPPY:     p.ob = 0.40f; p.vo = -8 * K; break;                 // ^_^
    case EMO_ANGRY:     p.ry = 60 * K; p.brow = 0.55f; p.red = true; break;
    case EMO_SURPRISED: p.rx = 55 * K; p.ry = 72 * K; p.sp = 186 * K; break;
    case EMO_SLEEPY:    p.ot = 0.28f; p.ob = 0.82f; p.vo = 8 * K; break;
    case EMO_SCARED:    p.rx = 62 * K; p.ry = 88 * K; p.sp = 205 * K; p.vo = -8 * K; break;
    case EMO_LOVE:      p.rx = 50 * K; p.ry = 50 * K; p.sp = 160 * K; p.heart = true; break;
    // not in Lumo, drawn in the same language:
    case EMO_SAD:       p.ry = 58 * K; p.ot = 0.92f; p.brow = -0.50f; p.vo = 10 * K; break;
    case EMO_CONFUSED:  p.asym = 1.0f; break;
    default: break;
  }
  return p;
}

// Lumo's transition speeds (main.cpp triggerEmotion calls)
static float rateFor(Emotion e) {
  switch (e) {
    case EMO_ANGRY: case EMO_SURPRISED: case EMO_SCARED: return 1.0f;
    case EMO_LOVE: return 0.5f;
    default: return 0.12f;
  }
}

static EyeParams cur = base(), tgt = base();
static float rate = 0.12f;
static Emotion emotion = EMO_NEUTRAL;
static uint32_t emotionUntil = 0;
static FaceState state = ST_IDLE;

static float lookX = 0, lookY = 0, lookTX = 0, lookTY = 0;   // -1..1
static bool  lookOverride = false;
static uint32_t nextLook = 1500;

static bool blinking = false; static uint32_t blinkStart = 0, nextBlink = 2000;
static const uint32_t BLINK_MS = 220;
static float blinkTop = 1, blinkBot = 1;                     // openness used while blinking

static int16_t shakeAmp = 0; static uint32_t shakeStart = 0, shakeDur = 0;
static float talk = 0, talkTarget = 0; static uint32_t talkStamp = 0;
static float micLevel = 0, micSlow = 0, ring = 0;
static uint32_t lastPoke = 0;
static float t = 0;

static inline float lerpf(float a, float b, float k) { return a + (b - a) * k; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float easeInOut(float x) { x = clampf(x, 0, 1); return x * x * (3 - 2 * x); }
static inline uint16_t rgb(float r, float g, float b) {
  int R = (int)clampf(r, 0, 255), G = (int)clampf(g, 0, 255), B = (int)clampf(b, 0, 255);
  uint16_t c = ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3);
  return (c >> 8) | (c << 8);      // panel wants big-endian
}
static void shake(int16_t amp, uint32_t ms) { shakeAmp = amp; shakeStart = millis(); shakeDur = ms; }

// ------------------------------------------------------------------ public API (unchanged)
void face_init() { lastPoke = millis(); nextBlink = millis() + 1500 + random(2001); }

void face_set_emotion(Emotion e, uint32_t ms) {
  if (e >= EMO_COUNT) return;
  bool changed = (e != emotion);
  emotion = e;
  emotionUntil = ms ? millis() + ms : 0;
  rate = rateFor(e);
  if (changed && e == EMO_ANGRY) shake(4, 2500);             // Lumo: subtle shiver of irritation
  if (changed && e == EMO_SCARED) shake(14, 1200);
  if (changed && e != EMO_NEUTRAL && !lookOverride) { lookTX = 0; lookTY = 0; }
}
void face_set_state(FaceState s) {
  if (s != ST_SLEEPING) face_poke();
  state = s;
  display_set_backlight(s == ST_SLEEPING ? 15 : 85);
}
FaceState face_get_state() { return state; }
void face_set_mouth(int level) { talkTarget = clampf(level / 100.0f, 0, 1); talkStamp = millis(); }
void face_set_mic_level(float l) { micLevel = clampf(l, 0, 1); }
void face_look(float x, float y) { lookOverride = true; lookTX = clampf(x, -1, 1); lookTY = clampf(y, -1, 1); }
void face_look_release() { lookOverride = false; }
void face_blink() { if (!blinking) { blinking = true; blinkStart = millis(); } }
void face_poke() {
  lastPoke = millis();
  if (state == ST_SLEEPING) { state = ST_IDLE; display_set_backlight(85); face_blink(); }
}

Emotion emotion_from_name(const char* s, bool* ok) {
  static const char* names[EMO_COUNT] = {"neutral","happy","sad","angry","surprised","love","confused","sleepy","scared"};
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
  if (emotionUntil && now > emotionUntil) { face_set_emotion(EMO_NEUTRAL, 0); }

  // sound in the room (same rules as before)
  float prevSlow = micSlow;
  micSlow = lerpf(micSlow, micLevel, 1.0f - expf(-dt * 3.0f));
  if (state == ST_SLEEPING && micLevel > 0.45f) face_poke();
  if (state == ST_IDLE) {
    if (micLevel > 0.55f && prevSlow < 0.08f && emotion == EMO_NEUTRAL) {   // sudden sound (Lumo: clap)
      face_set_emotion(EMO_SURPRISED, 2000); lastPoke = now;
    }
    if (micSlow > 0.10f) lastPoke = now;
  }
  uint32_t idle = now - lastPoke;
  if (state == ST_IDLE && idle > 180000) face_set_state(ST_SLEEPING);

  // target shape
  EyeParams g = preset(emotion);
  if (state == ST_IDLE && idle > 90000 && emotion == EMO_NEUTRAL) g = preset(EMO_SLEEPY);
  if (state == ST_LISTENING) { g.rx += 3; g.ry += 6; }
  if (state == ST_THINKING)  { g.ot = fminf(g.ot, 0.72f); }
  if (state == ST_SLEEPING)  { g = preset(EMO_SLEEPY); g.ot = 0.53f; g.ob = 0.53f; g.vo = 10 * K; g.red = false; }
  tgt = g;

  // Lumo: frame-rate independent lerp
  float a = clampf(rate * (dt * 1000.0f / 16.67f), 0, 1);
  cur.rx = lerpf(cur.rx, tgt.rx, a);  cur.ry = lerpf(cur.ry, tgt.ry, a);
  cur.sp = lerpf(cur.sp, tgt.sp, a);  cur.vo = lerpf(cur.vo, tgt.vo, a);
  cur.brow = lerpf(cur.brow, tgt.brow, a);
  cur.asym = lerpf(cur.asym, tgt.asym, a);
  cur.ot = lerpf(cur.ot, tgt.ot, a);  cur.ob = lerpf(cur.ob, tgt.ob, a);
  cur.heart = tgt.heart; cur.red = tgt.red;                  // Lumo switches these instantly

  // blink: lids meet at 0.52 and leave a thin stripe (Lumo's cartoon "closed" look)
  if (!blinking && now >= nextBlink && state != ST_SLEEPING) face_blink();
  blinkTop = cur.ot; blinkBot = cur.ob;
  if (blinking) {
    uint32_t el = now - blinkStart;
    if (el >= BLINK_MS) { blinking = false; nextBlink = now + 1500 + random(2001); }
    else {
      float k = (float)el / BLINK_MS;
      float c = k < 0.5f ? easeInOut(k * 2) : easeInOut((1 - k) * 2);
      const float M = 0.52f;
      blinkTop = lerpf(cur.ot, fminf(cur.ot, M), c);
      blinkBot = lerpf(cur.ob, fminf(cur.ob, M), c);
    }
  }

  // where to look (Lumo idle: 40% back to centre, else a random point; blink on 60% of moves)
  if (!lookOverride) {
    if (state == ST_THINKING) { lookTX = 0.7f; lookTY = -0.9f; }
    else if (state == ST_SLEEPING) { lookTX = 0; lookTY = 0.3f; }
    else if (emotion == EMO_NEUTRAL && now >= nextLook) {
      if (random(100) < 40) { lookTX = 0; lookTY = 0; }
      else { lookTX = random(-700, 701) / 1000.0f; lookTY = random(-300, 301) / 1000.0f; }
      if (random(100) < 60) face_blink();
      nextLook = now + 1500 + random(3501);
    }
  }
  float la = clampf(0.25f * (dt * 1000.0f / 16.67f), 0, 1);  // Lumo saccade speed
  lookX = lerpf(lookX, lookTX, la);
  lookY = lerpf(lookY, lookTY, la);

  if (now - talkStamp > 300 || state != ST_SPEAKING) talkTarget = 0;
  talk = lerpf(talk, talkTarget, 1.0f - expf(-dt * 25.0f));
  float ringT = (state == ST_LISTENING) ? 0.35f + micLevel * 0.65f : 0;
  ring = lerpf(ring, ringT, 1.0f - expf(-dt * 15.0f));
}

// ------------------------------------------------------------------ band primitives
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
  y0 = (int)floorf(top); if (y0 < BY0) y0 = BY0;
  y1 = (int)ceilf(bot) - 1; if (y1 > BY0 + BH - 1) y1 = BY0 + BH - 1;
  return y1 >= y0;
}
static void fillRect(float x, float y, float w, float h, uint16_t c) {
  int y0, y1; if (w <= 0 || h <= 0 || !rowRange(y, y + h, y0, y1)) return;
  for (int yy = y0; yy <= y1; yy++) span(yy, (int)lroundf(x), (int)lroundf(x + w) - 1, c);
}
static void fillRoundRect(float x, float y, float w, float h, float r, uint16_t c) {
  if (w < 1 || h < 1) return;
  r = fminf(r, fminf(w, h) * 0.5f);
  float L = x, R = x + w, T = y, Bo = y + h;
  int y0, y1; if (!rowRange(T, Bo, y0, y1)) return;
  for (int yy = y0; yy <= y1; yy++) {
    float yc = yy + 0.5f, inset = 0;
    if (yc < T + r) { float d = T + r - yc; inset = r - sqrtf(fmaxf(r * r - d * d, 0)); }
    else if (yc > Bo - r) { float d = yc - (Bo - r); inset = r - sqrtf(fmaxf(r * r - d * d, 0)); }
    span(yy, (int)lroundf(L + inset), (int)lroundf(R - inset) - 1, c);
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
// right triangle cut into the top of the eye; `rightCorner` = which top corner it drops on
static void wedge(float L, float R, float T, float reach, bool rightCorner, uint16_t c) {
  if (reach < 0.5f) return;
  int y0, y1; if (!rowRange(T, T + reach, y0, y1)) return;
  for (int y = y0; y <= y1; y++) {
    float f = 1.0f - (y + 0.5f - T) / reach;           // 1 at top, 0 at tip
    if (f <= 0) continue;
    float w = (R - L) * f;
    if (rightCorner) span(y, (int)lroundf(R - w), (int)lroundf(R), c);
    else             span(y, (int)lroundf(L), (int)lroundf(L + w), c);
  }
}
static void drawHeart(int cx, int cy) {
  int x0 = cx - HEART_W / 2, y0 = cy - HEART_H / 2;
  int ys = y0 > BY0 ? y0 : BY0, ye = (y0 + HEART_H < BY0 + BH) ? y0 + HEART_H : BY0 + BH;
  for (int y = ys; y < ye; y++) {
    int py = y - y0;
    uint16_t* row = B + (y - BY0) * LCD_W;
    for (int px = 0; px < HEART_W; px++) {
      int x = x0 + px; if (x < 0 || x >= LCD_W) continue;
      int i = py * HEART_W + px;
      if (heart_mask[i >> 3] & (0x80 >> (i & 7))) row[x] = heart_px[i];
    }
  }
}

// ------------------------------------------------------------------ one eye (Lumo drawEye)
static void drawEye(float cx, float cy, float rx, float ry, float ot, float ob, float brow,
                    bool isLeft, uint16_t col, uint16_t bg) {
  fillRoundRect(cx - rx, cy - ry, rx * 2, ry * 2, rx, col);   // capsule: radius = half width
  float top = cy - ry, bot = cy + ry, d = ry * 2;
  float lx = cx - rx - 2, lw = rx * 2 + 4;
  float th = (1 - ot) * d; if (th > 0.5f) fillRect(lx, top, lw, th, bg);
  float bh = (1 - ob) * d; if (bh > 0.5f) fillRect(lx, bot - bh, lw, bh, bg);
  if (brow > 0.01f)  wedge(lx, lx + lw, top - 2, brow * ry * 1.3f, isLeft, bg);    // angry: inner corner
  if (brow < -0.01f) wedge(lx, lx + lw, top - 2, -brow * ry * 1.3f, !isLeft, bg);  // sad: outer corner
}

// ------------------------------------------------------------------ one band
static void drawBand() {
  const uint16_t bg = rgb(0, 0, 0);
  for (int i = 0; i < LCD_W * BH; i++) B[i] = bg;

  float br = (state == ST_SLEEPING) ? 0.5f : 1.0f;
  uint16_t col = cur.red ? rgb(255 * br, 0, 0) : rgb(255 * br, 165 * br, 0);   // Lumo orange / red

  if (ring > 0.03f) {
    float rth = 4 + ring * 14;
    fillRing(180, 180, 179, 179 - rth, cur.red ? rgb(255 * ring, 0, 0) : rgb(255 * ring, 165 * ring, 0));
  }

  // shake (amplitude fades to 0 over its duration), stronger while yelling
  int sx = 0, sy = 0;
  int amp = 0;
  if (shakeAmp > 0) {
    uint32_t el = millis() - shakeStart;
    if (el < shakeDur) amp = (int)(shakeAmp * (1.0f - (float)el / shakeDur)); else shakeAmp = 0;
  }
  if (emotion == EMO_ANGRY && state == ST_SPEAKING) amp = max(amp, (int)(2 + 4 * talk));
  if (amp > 0) { sx = random(-amp, amp + 1); sy = random(-amp, amp + 1); }

  float cx = 180 + sx + lookX * 32 * K;
  float cy = 180 - 30 * K + cur.vo + sy + lookY * 20 * K;
  float half = cur.sp / 2;

  if (cur.heart && state != ST_SLEEPING) {
    drawHeart((int)lroundf(cx - half), (int)lroundf(cy));
    drawHeart((int)lroundf(cx + half), (int)lroundf(cy));
  } else {
    float s = cur.asym;
    // left eye: squints from the top when confused
    drawEye(cx - half, cy, cur.rx, cur.ry, fminf(blinkTop, 1 - 0.42f * s), blinkBot, cur.brow, true, col, bg);
    // right eye: a bit bigger when confused
    drawEye(cx + half, cy - 3 * s, cur.rx + 4 * s, cur.ry + 6 * s, blinkTop, blinkBot, cur.brow, false, col, bg);
  }

  // Lumo has no mouth; Laboo gets one only while it talks or thinks
  float my = cy + cur.ry + 46;
  if (state == ST_SPEAKING) {
    float w = 44 + 16 * talk, h = 8 + 34 * talk;
    fillRoundRect(cx - w / 2, my - h / 2, w, h, fminf(w, h) / 2, col);
  } else if (state == ST_THINKING) {
    for (int i = 0; i < 3; i++) {
      float r = 4 + 3 * fmaxf(0, sinf(t * 6 - i * 0.9f));
      fillEllipse(cx - 20 + i * 20, my, r, r, col);
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
