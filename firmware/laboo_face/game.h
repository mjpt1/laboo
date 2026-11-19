#pragma once
#include <Arduino.h>

// One-touch games. While a game runs it owns the screen and the taps.
//   dice      : swipe left/right on the face. Tap to roll again.
//   reaction  : swipe up/down. Wait for green eyes, then tap as fast as you can.
enum Game { GAME_NONE, GAME_DICE, GAME_REACTION };

void game_start(Game g);
Game game_active();
void game_stop();                 // long press: leave any game
void game_tap();                 // a tap while a game runs
void game_update(float dt);      // returns to GAME_NONE by itself when finished
bool game_draws_face();          // true: render the face screen; false: game_render_frame()
void game_render_frame();
