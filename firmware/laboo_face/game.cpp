#include "game.h"
#include "face.h"
#include "sound.h"
#include "display.h"
#include "ui.h"
#include <math.h>

static Game game = GAME_NONE;

// ------------------------------------------------------------------ dice
static int   diceVal = 1;
static float diceT = 0, diceNext = 0, diceShow = 0;   // rolling time, next face change, time shown after stop
static bool  rolling = false;
static const float ROLL_LEN = 1.4f;

static void diceRoll() { rolling = true; diceT = 0; diceNext = 0; diceShow = 0; sound_play(SND_DICE); }

// ------------------------------------------------------------------ reaction
enum RPhase { R_WAIT, R_GO, R_RESULT };
static RPhase rphase;
static float  rT = 0, rWait = 0;
static uint32_t goAt = 0; static int resultMs = 0;

void game_start(Game g) {
  game = g;
  if (g == GAME_DICE) diceRoll();
  if (g == GAME_REACTION) {
    rphase = R_WAIT; rT = 0; rWait = 1.5f + random(3000) / 1000.0f;
    face_set_tint(0); face_set_emotion(EMO_SLEEPY, 0);         // squinting: "get ready"
    face_caption(TX_WAIT, 60000);
  }
}
Game game_active() { return game; }

static void endGame() {
  game = GAME_NONE;
  face_set_tint(0); face_caption(-1, 0);
}
void game_stop() { if (game != GAME_NONE) { endGame(); face_set_emotion(EMO_NEUTRAL, 0); } }

void game_tap() {
  if (game == GAME_DICE) { if (!rolling) diceRoll(); return; }
  if (game == GAME_REACTION) {
    if (rphase == R_WAIT) {                                       // too early!
      sound_play(SND_LOSE); face_set_emotion(EMO_ANGRY, 2500); face_caption(TX_TOO_EARLY, 2500);
      game = GAME_NONE;
    } else if (rphase == R_GO) {
      resultMs = (int)(millis() - goAt);
      rphase = R_RESULT; rT = 0;
      face_set_tint(0); face_caption(-1, 0);
      if (resultMs < 330) sound_play(SND_WIN); else sound_play(SND_POP);
    } else if (rphase == R_RESULT && rT > 0.8f) {                 // tap on the result: play again
      game_start(GAME_REACTION);
    }
  }
}

void game_update(float dt) {
  if (game == GAME_DICE) {
    if (rolling) {
      diceT += dt;
      if (diceT >= diceNext) {                                    // faces change slower and slower
        int v; do { v = 1 + random(6); } while (v == diceVal);
        diceVal = v;
        diceNext = diceT + 0.05f + 0.25f * (diceT / ROLL_LEN) * (diceT / ROLL_LEN);
      }
      if (diceT > ROLL_LEN) { rolling = false; sound_play(SND_POP); }
    } else {
      diceShow += dt;
      if (diceShow > 4) { endGame(); face_set_emotion(EMO_HAPPY, 2000); }
    }
  } else if (game == GAME_REACTION) {
    rT += dt;
    if (rphase == R_WAIT && rT > rWait) {
      rphase = R_GO; goAt = millis();
      face_set_tint(1); face_set_emotion(EMO_SURPRISED, 0); face_caption(TX_NOW, 60000);
      sound_play(SND_GO);
    } else if (rphase == R_GO && millis() - goAt > 3000) {      // fell asleep
      endGame(); face_set_emotion(EMO_SLEEPY, 2500); sound_play(SND_LOSE);
    } else if (rphase == R_RESULT && rT > 5) {
      endGame(); face_set_emotion(resultMs < 330 ? EMO_HAPPY : EMO_NEUTRAL, 2500);
    }
  }
}

bool game_draws_face() {
  return game == GAME_NONE || (game == GAME_REACTION && rphase != R_RESULT);
}

// ------------------------------------------------------------------ drawing
static float jit = 0;                                           // dice shake, picked once per frame
static void diceBand() {
  ui_clear(ui_rgb(0, 0, 0));
  uint16_t org = ui_rgb(255, 165, 0), blk = ui_rgb(0, 0, 0);
  float j = jit;
  float s = 170, x = 180 - s / 2 + j, y = 180 - s / 2 - j * 0.6f;
  ui_round_rect(x, y, s, s, 30, org);
  float c = 180 + j, cy = 180 - j * 0.6f, o = 46, r = 15;
  auto pip = [&](float dx, float dy) { ui_ellipse(c + dx, cy + dy, r, r, blk); };
  int v = diceVal;
  if (v == 1 || v == 3 || v == 5) pip(0, 0);
  if (v >= 2) { pip(-o, -o); pip(o, o); }
  if (v >= 4) { pip(o, -o); pip(-o, o); }
  if (v == 6) { pip(-o, 0); pip(o, 0); }
}
static void resultBand() {
  ui_clear(ui_rgb(0, 0, 0));
  bool good = resultMs < 330;
  if (good) ui_set_text_color(40, 230, 90); else ui_set_text_color(255, 165, 0);
  int w = ui_number_width(UI_BIG, resultMs);
  ui_number(UI_BIG, resultMs, 180 - w / 2, 100);
  ui_text_centered(TX_MS, 180, 208);
  if (good) ui_text_centered(TX_BRAVO, 180, 262);
}

void game_render_frame() {
  jit = rolling ? random(-60, 61) / 10.0f : 0;                // shaking while it rolls
  for (int y0 = 0; y0 < LCD_H; y0 += BAND_H) {
    ui_band(display_band_buffer(), y0, BAND_H);
    if (game == GAME_DICE) diceBand(); else resultBand();
    display_push_band(y0, BAND_H);
  }
}
