// LABOO face, Lumo style (v3 test).
// Eyes ported from Lumo (github.com/Aymo/Lumo, src/eye_engine.cpp + emotion.cpp, GPL-3.0):
// pupil-less capsule eyes; expressions come only from the capsule shape, black
// eyelids from top and bottom, and a black wedge "eyebrow" cut into the top.
// All Lumo sizes were made for a 466 px screen and are scaled here to 360 px.
//
// Laboo extras: the USB command API in
// face.h, listening ring, a small mouth only while speaking, thinking dots,
// sleeping, and the two emotions Lumo doesn't have (sad, confused).
// Drawn band by band (40 rows at a time) straight into DMA RAM, like before.
#include "face.h"
#include "display.h"
#include "heart_lumo.h"
#include "ui.h"
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
static float micLevel = 0, ring = 0, attention = 0, attentionT = 0;
static uint32_t lastPoke = 0;
static float t = 0;
// desk-robot extras
static float bounceY = 0, bounceV = 0, swayX = 0, swayV = 0;   // little springs used for dancing
static bool  dancing = false;
static float yawnT = -1;                                     // >= 0 while yawning (seconds into it)
static const float YAWN_LEN = 2.6f;
static int   captionId = -1; static uint32_t captionUntil = 0;
static int   tint = 0;                                       // 0 orange, 1 green

static inline float lerpf(float a, float b, float k) { return a + (b - a) * k; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float easeInOut(float x) { x = clampf(x, 0, 1); return x * x * (3 - 2 * x); }
static inline uint16_t rgb(float r, float g, float b) {
  int R = (int)clampf(r, 0, 255), G = (int)clampf(g, 0, 255), B = (int)clampf(b, 0, 255);
  uint16_t c = ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3);
  return (c >> 8) | (c << 8);      // panel wants big-endian
}
static int shakeX = 0, shakeY = 0;
static Pose pose = POSE_NONE;
static uint32_t poseStart = 0, poseUntil = 0;
void face_pose(Pose p, uint32_t ms) { pose = p; poseStart = millis(); poseUntil = p == POSE_NONE ? 0 : millis() + ms; }
Pose face_get_pose() { return pose; }
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
void face_set_attention(bool on) { attentionT = on ? 1.0f : 0.0f; }
void face_bounce(float px) { bounceV -= px * 18; }
void face_sway(float px) { swayV += px * 18; }
void face_set_dance(bool on) { dancing = on; }
void face_yawn() { yawnT = 0; }
bool face_is_yawning() { return yawnT >= 0; }
void face_caption(int id, uint32_t ms) { captionId = id; captionUntil = millis() + ms; }
void face_set_tint(int tnt) { tint = tnt; }
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

  // Sound and idle reactions live in main.cpp now (desk-robot mode).
  // Here we only keep the "someone is talking" attention look.
  attention = lerpf(attention, attentionT, 1.0f - expf(-dt * 4.0f));

  // target shape
  EyeParams g = preset(emotion);
  if (state == ST_LISTENING) { g.rx += 3; g.ry += 6; }
  if (emotion == EMO_NEUTRAL) { g.rx += 3 * attention; g.ry += 6 * attention; }
  if (state == ST_THINKING)  { g.ot = fminf(g.ot, 0.72f); }
  if (state == ST_SLEEPING)  { g = preset(EMO_SLEEPY); g.ot = 0.53f; g.ob = 0.53f; g.vo = 10 * K; g.red = false; }
  if (dancing && emotion == EMO_NEUTRAL) { g.ob = 0.55f; g.vo = -6 * K; }        // happy-ish while dancing
  if (yawnT >= 0) {
    yawnT += dt;
    float k = sinf(fminf(yawnT / YAWN_LEN, 1.0f) * 3.1416f);   // 0 -> 1 -> 0
    g.ot = 1 - 0.78f * k; g.ob = 1 - 0.20f * k; g.vo += 6 * k;
    if (yawnT > YAWN_LEN) yawnT = -1;
  }
  tgt = g;
  if (yawnT >= 0) rate = fmaxf(rate, 0.2f);

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
    if (el >= BLINK_MS) { blinking = false; nextBlink = now + 3500 + random(3001); }
    else {
      float k = (float)el / BLINK_MS;
      float c = k < 0.5f ? easeInOut(k * 2) : easeInOut((1 - k) * 2);
      const float M = 0.52f;
      blinkTop = lerpf(cur.ot, fminf(cur.ot, M), c);
      blinkBot = lerpf(cur.ob, fminf(cur.ob, M), c);
    }
  }

  // poses
  if (pose != POSE_NONE && now > poseUntil) pose = POSE_NONE;
  if (pose == POSE_GIRL) blinkBot = fminf(blinkBot, 0.80f);   // a soft smile under the eyes

  // where to look (Lumo idle: 40% back to centre, else a random point; blink on 60% of moves)
  if (!lookOverride) {
    if (state == ST_THINKING) { lookTX = 0.7f; lookTY = -0.9f; }
    else if (state == ST_SLEEPING) { lookTX = 0; lookTY = 0.3f; }
    else if (attention > 0.5f && emotion == EMO_NEUTRAL) { lookTX = 0; lookTY = -0.15f; }  // looks at you
    else if (emotion == EMO_NEUTRAL && now >= nextLook) {
      if (random(100) < 40) { lookTX = 0; lookTY = 0; }
      else { lookTX = random(-400, 401) / 1000.0f; lookTY = random(-150, 151) / 1000.0f; }   // calmer: smaller moves
      if (random(100) < 25) face_blink();
      nextLook = now + 4000 + random(5001);                                                    // every 4-9 s
    }
  }
  float la = clampf(0.07f * (dt * 1000.0f / 16.67f), 0, 1);  // slow glide instead of a snap
  lookX = lerpf(lookX, lookTX, la);
  lookY = lerpf(lookY, lookTY, la);

  if (now - talkStamp > 300 || state != ST_SPEAKING) talkTarget = 0;
  talk = lerpf(talk, talkTarget, 1.0f - expf(-dt * 25.0f));
  float ringT = (state == ST_LISTENING) ? 0.35f + micLevel * 0.65f : 0;
  ring = lerpf(ring, ringT, 1.0f - expf(-dt * 15.0f));

  // springs for the dance bounce / sway (critically-ish damped)
  bounceV += (-120 * bounceY - 11 * bounceV) * dt; bounceY += bounceV * dt;
  swayV   += (-60 * swayX - 8 * swayV) * dt;      swayX   += swayV * dt;
  if (captionId >= 0 && now > captionUntil) captionId = -1;

  // shake (amplitude fades to 0 over its duration), stronger while yelling
  int amp = 0;
  if (shakeAmp > 0) {
    uint32_t el = now - shakeStart;
    if (el < shakeDur) amp = (int)(shakeAmp * (1.0f - (float)el / shakeDur)); else shakeAmp = 0;
  }
  if (emotion == EMO_ANGRY && state == ST_SPEAKING) amp = max(amp, (int)(2 + 4 * talk));
  shakeX = amp > 0 ? random(-amp, amp + 1) : 0;
  shakeY = amp > 0 ? random(-amp, amp + 1) : 0;
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
// ---- anti-aliased shapes: 4 sub-rows per pixel row, exact horizontal coverage, blended edges
static float gAlpha = 1.0f;                       // global opacity for the next shapes (blush, fading hearts)
static inline uint16_t blend565(uint16_t dst, uint16_t src, int a) {   // both big-endian, a 0..256
  uint16_t d = (dst >> 8) | (dst << 8), s = (src >> 8) | (src << 8);
  int r = (((s >> 11) & 31) * a + ((d >> 11) & 31) * (256 - a)) >> 8;
  int g = (((s >> 5) & 63) * a + ((d >> 5) & 63) * (256 - a)) >> 8;
  int b = ((s & 31) * a + (d & 31) * (256 - a)) >> 8;
  uint16_t o = (r << 11) | (g << 5) | b;
  return (o >> 8) | (o << 8);
}
static inline uint16_t mix565(uint16_t c1, uint16_t c2, float k) { return blend565(c1, c2, (int)(clampf(k, 0, 1) * 256)); }
typedef bool (*RowFn)(float y, const float* p, float& xl, float& xr);
// fills a shape whose rows are given by f; colour goes from cTop to cBot down the shape
static void fillShapeAA(float top, float bot, RowFn f, const float* p, uint16_t cTop, uint16_t cBot) {
  static float cov[LCD_W];
  int y0, y1; if (!rowRange(top, bot + 1, y0, y1)) return;
  float hgt = fmaxf(bot - top, 1);
  for (int y = y0; y <= y1; y++) {
    float XL[4], XR[4]; int ns = 0;
    float maxL = -1e9f, minR = 1e9f;
    for (int s = 0; s < 4; s++) {
      float yy = y + 0.125f + 0.25f * s, xl, xr;
      if (!f(yy, p, xl, xr)) continue;
      if (xl < 0) xl = 0; if (xr > LCD_W) xr = LCD_W; if (xr <= xl) continue;
      XL[ns] = xl; XR[ns] = xr; ns++;
      if (xl > maxL) maxL = xl; if (xr < minR) minR = xr;
    }
    if (!ns) continue;
    uint16_t c = cTop == cBot ? cTop : mix565(cTop, cBot, (y + 0.5f - top) / hgt);
    uint16_t* row = B + (y - BY0) * LCD_W;
    // solid middle: pixels every sub-row covers fully -> plain fill, no maths
    int iL = LCD_W, iR = -1;
    if (ns == 4 && gAlpha >= 0.999f) { iL = (int)ceilf(maxL); iR = (int)floorf(minR) - 1; }
    if (iR >= iL) for (int px = iL; px <= iR; px++) row[px] = c;
    // edges: exact coverage, only outside the solid middle
    int mn = LCD_W, mx = -1;
    for (int s = 0; s < ns; s++) {
      int a = (int)XL[s], b = (int)ceilf(XR[s]) - 1;
      for (int px = a; px <= b; px++) {
        if (px >= iL && px <= iR) { px = iR; continue; }
        float o = fminf(px + 1, XR[s]) - fmaxf(px, XL[s]);
        if (o > 0) cov[px] += 0.25f * o;
      }
      if (a < mn) mn = a; if (b > mx) mx = b;
    }
    for (int px = mn; px <= mx; px++) {
      if (px >= iL && px <= iR) { px = iR; continue; }
      int a = (int)(cov[px] * gAlpha * 256); cov[px] = 0;
      if (a >= 255) row[px] = c; else if (a > 0) row[px] = blend565(row[px], c, a);
    }
  }
}
static bool rowEllipse(float y, const float* p, float& xl, float& xr) {   // p: cx cy rx ry
  float d = (y - p[1]) / p[3]; if (d * d >= 1) return false;
  float dx = p[2] * sqrtf(1 - d * d); xl = p[0] - dx; xr = p[0] + dx; return true;
}
static bool rowRound(float y, const float* p, float& xl, float& xr) {     // p: x y w h r
  if (y < p[1] || y > p[1] + p[3]) return false;
  float r = p[4], inset = 0;
  if (y < p[1] + r) { float d = p[1] + r - y; inset = r - sqrtf(fmaxf(r * r - d * d, 0)); }
  else if (y > p[1] + p[3] - r) { float d = y - (p[1] + p[3] - r); inset = r - sqrtf(fmaxf(r * r - d * d, 0)); }
  xl = p[0] + inset; xr = p[0] + p[2] - inset; return xr > xl;
}
static void fillRoundRectG(float x, float y, float w, float h, float r, uint16_t c1, uint16_t c2) {
  if (w < 1 || h < 1) return;
  float p[5] = {x, y, w, h, fminf(r, fminf(w, h) * 0.5f)};
  fillShapeAA(y, y + h, rowRound, p, c1, c2);
}
static void fillRoundRect(float x, float y, float w, float h, float r, uint16_t c) { fillRoundRectG(x, y, w, h, r, c, c); }
static void fillEllipseG(float cx, float cy, float rx, float ry, uint16_t c1, uint16_t c2) {
  if (rx < 0.3f || ry < 0.3f) return;
  float p[4] = {cx, cy, rx, ry};
  fillShapeAA(cy - ry, cy + ry, rowEllipse, p, c1, c2);
}
static void fillEllipse(float cx, float cy, float rx, float ry, uint16_t c) { fillEllipseG(cx, cy, rx, ry, c, c); }
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

// ---- pose drawing helpers (all anti-aliased)
// tapered stroke along a quadratic curve p0 -> p2 bending towards p1, radius r0 -> r1
static void curveStroke(float x0, float y0, float x1, float y1, float x2, float y2, float r0, float r1, uint16_t c) {
  float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)) + sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
  int n = (int)(len / fmaxf(1.2f, fminf(r0, r1) * 0.8f)) + 2;   // dots overlap enough to look smooth
  // skip the whole stroke if it is nowhere near this band
  float ymin = fminf(y0, fminf(y1, y2)) - r0 - 1, ymax = fmaxf(y0, fmaxf(y1, y2)) + r0 + 1;
  if (ymax < BY0 || ymin > BY0 + BH) return;
  for (int i = 0; i <= n; i++) {
    float t = (float)i / n, a = (1 - t) * (1 - t), b = 2 * (1 - t) * t, d = t * t;
    float r = r0 + (r1 - r0) * t;
    fillEllipse(a * x0 + b * x1 + d * x2, a * y0 + b * y1 + d * y2, r, r, c);
  }
}
// half-lens: flat edge on y = p[1], curved edge reaching h away (p[4] = +1 below, -1 above), width p[2]
static bool rowLens(float y, const float* p, float& xl, float& xr) {   // p: cx cy w h dir
  float t = (y - p[1]) * p[4] / p[3]; if (t < 0 || t >= 1) return false;
  float u = sqrtf(1 - t) * p[2] / 2; xl = p[0] - u; xr = p[0] + u; return true;
}
static void lens(float cx, float cy, float w, float h, float dir, uint16_t c1, uint16_t c2) {
  float p[5] = {cx, cy, w, h, dir};
  if (dir > 0) fillShapeAA(cy, cy + h, rowLens, p, c1, c2); else fillShapeAA(cy - h, cy, rowLens, p, c1, c2);
}
// heart (point down): two round lobes + a V; s = half width
static bool rowHeartV(float y, const float* p, float& xl, float& xr) {   // p: cx cy s
  float t = (y - (p[1] - 0.05f * p[2])) / (1.05f * p[2]); if (t < 0 || t >= 1) return false;
  float u = (1 - t) * 0.98f * p[2]; xl = p[0] - u; xr = p[0] + u; return true;
}
static void softHeart(float cx, float cy, float s, uint16_t c1, uint16_t c2) {
  fillEllipseG(cx - 0.5f * s, cy - 0.3f * s, 0.52f * s, 0.52f * s, c1, c2);
  fillEllipseG(cx + 0.5f * s, cy - 0.3f * s, 0.52f * s, 0.52f * s, c1, c2);
  float p[3] = {cx, cy, s};
  fillShapeAA(cy - 0.05f * s, cy + s, rowHeartV, p, mix565(c1, c2, 0.4f), c2);
  float ga = gAlpha; gAlpha *= 0.7f;
  fillEllipse(cx - 0.55f * s, cy - 0.45f * s, 0.16f * s, 0.11f * s, rgb(255, 230, 235));   // shine
  gAlpha = ga;
}
static void blush(float cx, float cy, float k) {
  float ga = gAlpha; gAlpha = 0.45f * k;
  fillEllipse(cx, cy, 26, 11, rgb(255, 90, 140));
  gAlpha = 0.35f * k;
  fillEllipse(cx, cy, 17, 7, rgb(255, 140, 175));
  gAlpha = ga;
}
// glossy red lips: upper lip with a cupid's bow, fuller lower lip, smiling corners
static void drawLips(float cx, float cy, float w, float k) {
  const uint16_t dk = rgb(130, 0, 30), up1 = rgb(200, 10, 50), up2 = rgb(170, 0, 40);
  const uint16_t lo1 = rgb(235, 30, 70), lo2 = rgb(180, 5, 45);
  float hu = 15 * k, hl = 21 * k;
  lens(cx, cy, w, hu * 0.75f, -1, up1, up2);                                    // upper lip body
  fillEllipseG(cx - w * 0.17f, cy - hu * 0.55f, w * 0.2f, hu * 0.55f, up1, up2); // two bow peaks
  fillEllipseG(cx + w * 0.17f, cy - hu * 0.55f, w * 0.2f, hu * 0.55f, up1, up2);
  lens(cx, cy - 1, w * 0.97f, hl, +1, lo1, lo2);                                // lower lip
  // mouth line: dark, corners turned up
  curveStroke(cx - w / 2 + 2, cy - 4, cx, cy + 4, cx + w / 2 - 2, cy - 4, 1.6f, 1.6f, dk);
  // gloss on the lower lip and a small one on the upper
  float ga = gAlpha; gAlpha = 0.75f;
  fillEllipse(cx - w * 0.12f, cy + hl * 0.42f, w * 0.13f, hl * 0.14f, rgb(255, 170, 185));
  gAlpha = 0.5f;
  fillEllipse(cx + w * 0.2f, cy - hu * 0.6f, w * 0.06f, hu * 0.12f, rgb(255, 170, 185));
  gAlpha = ga;
}
// puckered kiss lips, a little to the side
static void drawPucker(float cx, float cy, float k) {
  const uint16_t c1 = rgb(240, 40, 80), c2 = rgb(175, 0, 45);
  fillEllipseG(cx, cy - 7 * k, 13 * k, 9 * k, c1, c2);           // upper
  fillEllipseG(cx + 1, cy + 6 * k, 15 * k, 11 * k, c1, c2);      // lower, fuller
  fillEllipse(cx + 1, cy, 4.5f * k, 3.2f * k, rgb(110, 0, 25));  // tiny opening
  // pucker folds
  curveStroke(cx - 9 * k, cy - 2, cx - 12 * k, cy, cx - 9 * k, cy + 3, 0.9f, 0.9f, rgb(150, 0, 40));
  curveStroke(cx + 10 * k, cy - 2, cx + 13 * k, cy, cx + 10 * k, cy + 3, 0.9f, 0.9f, rgb(150, 0, 40));
  float ga = gAlpha; gAlpha = 0.7f;
  fillEllipse(cx - 5 * k, cy + 9 * k, 4 * k, 2.2f * k, rgb(255, 180, 195));
  gAlpha = ga;
}
// zipper across the mouth; k = 0..1 how far it is pulled
static void drawZip(float cx, float cy, float k) {
  const float L = cx - 62, R = cx + 62, x = L + (R - L) * easeInOut(k);
  fillRoundRectG(L - 6, cy - 10, R - L + 12, 20, 10, rgb(55, 20, 10), rgb(25, 8, 4));   // mouth slot
  for (float tx = L; tx <= R; tx += 7) {                      // teeth: meshed behind the slider
    float open = tx < x ? 0 : 3.5f;
    fillRoundRectG(tx, cy - 7 - open, 5, 6, 1.5f, rgb(250, 215, 120), rgb(190, 140, 50));
    fillRoundRectG(tx + 3.5f, cy + 1 + open, 5, 6, 1.5f, rgb(250, 215, 120), rgb(190, 140, 50));
  }
  fillRoundRectG(x - 10, cy - 12, 20, 24, 6, rgb(255, 225, 140), rgb(175, 125, 40));    // slider
  fillRoundRectG(x - 6, cy - 9, 7, 16, 3, rgb(255, 245, 210), rgb(240, 200, 110));     // slider shine
  fillRoundRectG(x - 5, cy + 10, 10, 22, 5, rgb(250, 210, 120), rgb(170, 120, 40));    // pull tab
  fillEllipse(x, cy + 25, 2.5f, 4, rgb(0, 0, 0));                                       // tab hole
}
// four-point sparkle / star of radius s
static void sparkle(float x, float y, float s, uint16_t c) {
  if (s < 1) return;
  for (int i = 0; i < 4; i++) {
    float a = i * 1.5708f + 0.4f;
    curveStroke(x, y, x + cosf(a) * s * 0.5f, y + sinf(a) * s * 0.5f, x + cosf(a) * s, y + sinf(a) * s, s * 0.22f, 0.6f, c);
  }
  fillEllipse(x, y, s * 0.25f, s * 0.25f, rgb(255, 255, 255));
}
// sunglasses over both eyes
static void drawShades(float cx, float cy, float half, float rx, float ry) {
  float w = rx * 2 + 30, h = ry * 1.25f;
  for (int e = 0; e < 2; e++) {
    float ex = e ? cx + half : cx - half;
    fillRoundRectG(ex - w / 2 - 3, cy - h / 2 - 3, w + 6, h + 6, 18, rgb(60, 60, 70), rgb(20, 20, 25));   // frame
    fillRoundRectG(ex - w / 2, cy - h / 2, w, h, 16, rgb(45, 45, 70), rgb(5, 5, 12));                     // lens
    float ga = gAlpha; gAlpha = 0.35f;                                                                     // shine stripe
    curveStroke(ex - w * 0.3f, cy + h * 0.2f, ex - w * 0.1f, cy - h * 0.05f, ex + w * 0.05f, cy - h * 0.32f, 4, 3, rgb(255, 255, 255));
    gAlpha = ga;
  }
  fillRoundRectG(cx - half + w / 2, cy - h / 2 + 4, 2 * half - w, 9, 4, rgb(70, 70, 80), rgb(30, 30, 35));  // bridge
}
// ---- emoji-style decorations (from the reference sheet)
static inline void stroke(float x0, float y0, float x1, float y1, float r, uint16_t c) {
  curveStroke(x0, y0, (x0 + x1) / 2, (y0 + y1) / 2, x1, y1, r, r, c);
}
static void smile(float cx, float y, float w, uint16_t c) { curveStroke(cx - w / 2, y - 4, cx, y + 10, cx + w / 2, y - 4, 3.6f, 3.6f, c); }
static void frown(float cx, float y, float w, uint16_t c) { curveStroke(cx - w / 2, y + 6, cx, y - 8, cx + w / 2, y + 6, 3.6f, 3.6f, c); }
// heart eye: yellow outline, red->pink inside
static void loveEye(float x, float y, float s, uint16_t outline) {
  softHeart(x, y, s, outline, outline);
  softHeart(x, y + s * 0.04f, s * 0.78f, rgb(255, 90, 110), rgb(225, 10, 50));
}
// "///" blush marks under an eye
static void blushMarks(float x, float y, float k) {
  for (int i = -1; i <= 1; i++) stroke(x + i * 9 - 3, y + 6, x + i * 9 + 3, y - 6, 2.6f * k, rgb(255, 90, 160));
}
// pink cheek
static void cheek(float x, float y, float k) {
  float ga = gAlpha; gAlpha = 0.85f * k;
  fillEllipse(x, y, 17, 11, rgb(255, 110, 150));
  gAlpha = ga;
}
// "!!" in the top right
static void bangs(float x, float y, float k, uint16_t c) {
  for (int i = 0; i < 2; i++) {
    float bx = x + i * 20, by = y + i * 8;
    stroke(bx, by, bx - 7 * k, by + 26 * k, 4.5f * k, c);
    fillEllipse(bx - 10 * k, by + 37 * k, 4.5f * k, 4.5f * k, c);
  }
}
// red anger mark (four bent strokes around a centre)
static void angerMark(float x, float y, float s) {
  const uint16_t r = rgb(240, 30, 30);
  for (int q = 0; q < 4; q++) {
    float sx = (q & 1) ? 1 : -1, sy = (q & 2) ? 1 : -1;
    float ax = x + sx * s * 0.25f, ay = y + sy * s;
    float bx = x + sx * s, by = y + sy * s * 0.25f;
    curveStroke(ax, ay, x + sx * s * 0.25f, y + sy * s * 0.25f, bx, by, 3, 3, r);
  }
}
// one "Z"
static void zee(float x, float y, float s, uint16_t c) {
  stroke(x - s, y - s, x + s, y - s, s * 0.28f, c);
  stroke(x + s, y - s, x - s, y + s, s * 0.28f, c);
  stroke(x - s, y + s, x + s, y + s, s * 0.28f, c);
}
// blue tear stream from under an eye
static void tear(float x, float y, float len, float t, float k) {
  const uint16_t c1 = rgb(110, 180, 255), c2 = rgb(40, 110, 255);
  float w = 13 * k;
  fillRoundRectG(x - w, y, w * 2, len * k, 6, c1, c2);
  for (int i = 0; i < 3; i++) {                                  // wavy bottom
    float bx = x - w + w * i, by = y + len * k + 2 * sinf(t * 8 + i * 2);
    fillEllipseG(bx + w * 0.0f, by, w * 0.6f, 6 * k, c1, c2);
  }
  float ga = gAlpha; gAlpha = 0.6f;
  fillRoundRect(x - w * 0.55f, y + 4, w * 0.35f, len * k * 0.6f, 3, rgb(210, 235, 255));   // shine
  gAlpha = ga;
}
// pink bow
static void bow(float x, float y, float k) {
  const uint16_t p1 = rgb(255, 120, 200), p2 = rgb(225, 40, 140), dk = rgb(170, 20, 100);
  float s = 26 * k;
  fillEllipseG(x - s * 0.95f, y - s * 0.2f, s * 0.95f, s * 0.7f, p1, p2);
  fillEllipseG(x + s * 0.95f, y - s * 0.2f, s * 0.95f, s * 0.7f, p1, p2);
  fillEllipseG(x - s * 0.55f, y + s * 0.75f, s * 0.35f, s * 0.55f, p1, p2);     // tails
  fillEllipseG(x + s * 0.55f, y + s * 0.75f, s * 0.35f, s * 0.55f, p1, p2);
  curveStroke(x - s * 1.4f, y - s * 0.3f, x - s * 0.8f, y - s * 0.05f, x - s * 0.3f, y, 1.8f, 1.2f, dk);   // folds
  curveStroke(x + s * 1.4f, y - s * 0.3f, x + s * 0.8f, y - s * 0.05f, x + s * 0.3f, y, 1.8f, 1.2f, dk);
  fillEllipseG(x, y - s * 0.05f, s * 0.36f, s * 0.42f, p1, dk);                 // knot
}
// "<" winking eye
static void chevron(float x, float y, float s, uint16_t c) {
  stroke(x + s * 0.6f, y - s, x - s * 0.6f, y, 6, c);
  stroke(x - s * 0.6f, y, x + s * 0.6f, y + s, 6, c);
}
// "3" kissing mouth
static void kissMouth(float x, float y, uint16_t c) {
  curveStroke(x - 6, y - 14, x + 14, y - 13, x, y, 3.2f, 3.2f, c);
  curveStroke(x, y, x + 14, y + 13, x - 6, y + 14, 3.2f, 3.2f, c);
}
// hug: a big heart held by two little hands
static void hugHeart(float x, float y, float k, uint16_t hand) {
  softHeart(x, y, 44 * k, rgb(255, 90, 110), rgb(220, 10, 50));
  for (int e = 0; e < 2; e++) {
    float sx = e ? 1 : -1, hx = x + sx * 40 * k, hy = y + 18 * k;
    fillEllipseG(hx, hy, 19 * k, 15 * k, rgb(255, 200, 40), rgb(230, 150, 0));
    for (int f = 0; f < 3; f++)                                         // fingers wrapped on the heart
      fillEllipseG(hx - sx * (12 + f * 2) * k, hy - (10 - f * 9) * k, 7 * k, 5 * k, rgb(255, 205, 50), rgb(235, 155, 0));
    (void)hand;
  }
}
// closed happy eye: an arc bending up
static void happyArc(float ex, float ey, float rx, uint16_t c) {
  curveStroke(ex - rx, ey + 6, ex, ey - 18, ex + rx, ey + 6, 5, 5, c);
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
  if (tint == 1) col = rgb(40, 230, 90);                                         // game: green "now!" eyes

  if (ring > 0.03f) {
    float rth = 4 + ring * 14;
    fillRing(180, 180, 179, 179 - rth, cur.red ? rgb(255 * ring, 0, 0) : rgb(255 * ring, 165 * ring, 0));
  }

  // shake (amplitude fades to 0 over its duration), stronger while yelling
  // offset is picked once per frame in face_update, so every band uses the same one (no tearing)
  int sx = shakeX, sy = shakeY;

  float cx = 180 + sx + lookX * 32 * K + swayX;
  float cy = 180 - 30 * K + cur.vo + sy + lookY * 20 * K + bounceY;
  float half = cur.sp / 2;

  float ptE = (millis() - poseStart) / 1000.0f;
  if (pose == POSE_DIZZY) { cx += 12 * sinf(ptE * 7); cy += 4 * sinf(ptE * 14); }      // head wobbles
  // emoji look when no pose is on: picked from the emotion
  bool plain = pose == POSE_NONE && captionId < 0 && tint == 0 && yawnT < 0 && state != ST_SPEAKING && state != ST_THINKING;
  bool sleepy = plain && (state == ST_SLEEPING || emotion == EMO_SLEEPY);
  bool happy  = (plain && emotion == EMO_HAPPY && !dancing) || pose == POSE_HUG;
  bool love   = plain && emotion == EMO_LOVE;
  if (happy) {                                              // ^ ^
    happyArc(cx - half, cy, cur.rx * 1.1f, col);
    happyArc(cx + half, cy, cur.rx * 1.1f, col);
  } else if (sleepy) {                                      // flat sleepy bars
    fillRoundRect(cx - half - cur.rx * 1.2f, cy + 6, cur.rx * 2.4f, 16, 8, col);
    fillRoundRect(cx + half - cur.rx * 1.2f, cy + 6, cur.rx * 2.4f, 16, 8, col);
  } else if (love || (cur.heart && state != ST_SLEEPING)) {
    float beat = 1 + 0.08f * fmaxf(0, sinf(t * 7));
    loveEye(cx - half, cy, 34 * beat, col);
    loveEye(cx + half, cy, 34 * beat, col);
  } else if (pose == POSE_WINK && ptE > 0.1f && ptE < 1.7f) {   // left eye open, right eye "<"
    drawEye(cx - half, cy, cur.rx, cur.ry, blinkTop, blinkBot, 0, true, col, bg);
    chevron(cx + half, cy, 24, col);
  } else if (pose == POSE_DIZZY) {                          // spiral eyes, spinning
    for (int e = 0; e < 2; e++) {
      float ex = e ? cx + half : cx - half, dir = e ? 1 : -1;
      for (float th = 0.3f; th < 12.0f; th += 0.18f) {
        float r = th * 2.6f, a = th * dir + ptE * 9;
        fillEllipse(ex + r * cosf(a), cy + r * sinf(a), 3.2f, 3.2f, col);
      }
    }
  } else {
    float s = cur.asym;
    // left eye: squints from the top when confused
    drawEye(cx - half, cy, cur.rx, cur.ry, fminf(blinkTop, 1 - 0.42f * s), blinkBot, cur.brow, true, col, bg);
    // right eye: a bit bigger when confused
    drawEye(cx + half, cy - 3 * s, cur.rx + 4 * s, cur.ry + 6 * s, blinkTop, blinkBot, cur.brow, false, col, bg);
  }

  // Lumo has no mouth; Laboo gets one only while it talks or thinks
  float my = cy + cur.ry + 46;
  float mm = cy + cur.ry + 30;                              // emoji mouth sits closer to the eyes
  if (state == ST_SPEAKING) {
    float w = 44 + 16 * talk, h = 8 + 34 * talk;
    fillRoundRect(cx - w / 2, my - h / 2, w, h, fminf(w, h) / 2, col);
  } else if (yawnT >= 0) {
    float k = sinf(fminf(yawnT / YAWN_LEN, 1.0f) * 3.1416f);
    if (k > 0.05f) {
      float rxm = 10 + 12 * k, rym = 6 + 26 * k;
      fillEllipse(cx, my + 6, rxm, rym, col);
      if (rym > 9) fillEllipse(cx, my + 6, rxm - 5, rym - 5, bg);      // open "O"
    }
  } else if (state == ST_THINKING) {
    for (int i = 0; i < 3; i++) {
      float r = 4 + 3 * fmaxf(0, sinf(t * 6 - i * 0.9f));
      fillEllipse(cx - 20 + i * 20, my, r, r, col);
    }
  }

  // emotion extras
  float et = t;                                             // running time for small animations
  if (plain && pose == POSE_NONE) {
    if (happy) {
      blushMarks(cx - half - 6, cy + 36, 1); blushMarks(cx + half + 6, cy + 36, 1);
    } else if (love) {
      smile(cx, mm + 8, 40, col);
    } else if (sleepy) {
      fillEllipse(cx, mm + 6, 9, 9, col); fillEllipse(cx, mm + 6, 4.5f, 4.5f, bg);   // little "o"
      for (int i = 0; i < 3; i++) {                                                    // zzz drifting up
        float k = fmodf(et * 0.5f + i / 3.0f, 1.0f);
        float ga = gAlpha; gAlpha = k < 0.75f ? 1 : (1 - k) / 0.25f;
        zee(cx + 70 + 22 * k + i * 4, cy - cur.ry - 10 - 40 * k, 5 + 4 * k, rgb(90, 150, 255));
        gAlpha = ga;
      }
    } else if (emotion == EMO_SURPRISED || emotion == EMO_SCARED) {
      fillEllipse(cx, mm + 10, 14, 17, col); fillEllipse(cx, mm + 10, 8, 11, bg);       // "O"
      bangs(cx + 92, cy - cur.ry - 34, 1, col);
    } else if (emotion == EMO_SAD) {
      tear(cx - half, cy + cur.ry * 0.7f, 52, et, 1);
      tear(cx + half, cy + cur.ry * 0.7f, 52, et + 1, 1);
      frown(cx, mm + 8, 36, col);
    } else if (emotion == EMO_ANGRY) {
      frown(cx, mm + 8, 36, col);
      angerMark(cx + 100, cy - cur.ry - 22, 11 + fmaxf(0, sinf(et * 9)) * 2);
    }
  }

  // poses
  float pt = (millis() - poseStart) / 1000.0f;
  float pin = fminf(pt / 0.35f, 1.0f);                        // grows in over 0.35 s
  float pk = easeInOut(pin) * (1.0f + 0.12f * sinf(fminf(pt / 0.35f, 1.0f) * 3.1416f));   // with a little pop
  if (pose == POSE_GIRL) {
    // curled lashes on the outer side of each eye
    for (int e = 0; e < 2; e++) {
      float ex = e ? cx + half : cx - half, sgn = e ? 1 : -1;
      for (int i = 0; i < 3; i++) {
        float by = cy - cur.ry * 0.75f + i * 11, bx = ex + sgn * cur.rx * (0.75f + 0.1f * i);
        float L = (17 - 2 * i) * pk, up = -0.9f + 0.45f * i;
        curveStroke(bx, by, bx + sgn * L * 0.7f, by + up * L * 0.3f, bx + sgn * L, by + up * L * 0.9f - 4, 3.0f, 1.0f, col);
      }
    }
    bow(cx + 88, cy - cur.ry - 30, pk);
  } else if (pose == POSE_KISS) {
    kissMouth(cx + 2, mm + 10, col);
    float k = fmodf(pt, 1.6f) / 1.6f;                        // heart floats up and fades
    float ga = gAlpha; gAlpha = k < 0.7f ? 1.0f : (1 - k) / 0.3f;
    softHeart(cx + half + 30 + 14 * k, mm - 10 - 50 * k, 13 + 4 * k, rgb(255, 100, 170), rgb(230, 30, 120));
    gAlpha = ga;
  } else if (pose == POSE_WINK) {
    smile(cx, mm + 8, 36, col);
    float s = 13 * pk * (1 + 0.1f * fmaxf(0, sinf(pt * 8)));
    softHeart(cx + half + 34, cy + cur.ry * 0.9f, s, rgb(255, 100, 170), rgb(230, 30, 120));
  } else if (pose == POSE_HUG) {
    cheek(cx - half - 22, cy + 28, pk); cheek(cx + half + 22, cy + 28, pk);
    float beat = 1 + 0.06f * fmaxf(0, sinf(pt * 7));
    hugHeart(cx, mm + 22, pk * beat, col);
  } else if (pose == POSE_ZIP) {
    drawZip(cx, my + 4, fminf(pt / 0.9f, 1.0f));
  } else if (pose == POSE_COOL) {
    // glasses drop from the top with a small bounce, then sit
    float k = fminf(pt / 0.55f, 1.0f);
    float drop = k < 1 ? (1 - k) * (1 - k) * -230 + 10 * sinf(k * 3.1416f) * (1 - k) : 0;
    drawShades(cx, cy + drop, half, cur.rx, cur.ry);
    smile(cx, mm + 10, 40, col);
    if (pt > 0.5f) sparkle(cx + 100, cy - cur.ry - 20, 12 * fminf((pt - 0.5f) / 0.3f, 1) * (1 + 0.15f * sinf(pt * 6)), rgb(255, 225, 90));
  } else if (pose == POSE_DIZZY) {
    for (int i = 0; i < 3; i++) {                                                    // stars orbit the head
      float a = pt * 3.2f + i * 2.094f;
      float sx2 = cx + 95 * cosf(a), sy2 = cy - cur.ry - 22 + 16 * sinf(a);
      sparkle(sx2, sy2, 9 + 2 * sinf(a), rgb(255, 225, 90));
    }
    curveStroke(cx - 24, my + 4, cx - 8, my - 6, cx + 4, my + 4, 3, 3, col);         // wavy mouth
    curveStroke(cx + 4, my + 4, cx + 16, my + 14, cx + 26, my + 2, 3, 3, col);
  }
  // caption under the eyes (صبح بخیر, حالا!, ...)
  if (captionId >= 0) {
    if (tint == 1) ui_set_text_color(40, 230, 90); else ui_set_text_color(255 * br, 165 * br, 0);
    ui_text_centered(captionId, 180, yawnT >= 0 ? 296 : 262);   // move below the yawning mouth
  }
}

void face_render_frame() {
  for (int y0 = 0; y0 < LCD_H; y0 += BAND_H) {
    B = display_band_buffer(); BY0 = y0; BH = BAND_H;
    ui_band(B, y0, BAND_H);
    drawBand();
    display_push_band(y0, BAND_H);
  }
}
