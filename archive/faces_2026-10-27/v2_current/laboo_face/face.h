#pragma once
#include <Arduino.h>

enum Emotion { EMO_NEUTRAL, EMO_HAPPY, EMO_SAD, EMO_ANGRY, EMO_SURPRISED, EMO_LOVE, EMO_CONFUSED, EMO_SLEEPY, EMO_COUNT };
enum FaceState { ST_IDLE, ST_LISTENING, ST_THINKING, ST_SPEAKING, ST_SLEEPING };

void face_init();
void face_update(float dt);          // advance animation
void face_render_frame();            // draw + push a whole frame to the LCD

void face_set_emotion(Emotion e, uint32_t duration_ms = 0);  // 0 = stays
void face_set_state(FaceState s);
FaceState face_get_state();
void face_set_mouth(int level);      // 0..100 (lip sync while speaking)
void face_set_mic_level(float lvl);  // 0..1 (ring while listening)
void face_look(float x, float y);    // -1..1, overrides idle eye movement
void face_look_release();
void face_poke();                    // any interaction: wakes up, resets sleep timer
void face_blink();

Emotion emotion_from_name(const char* s, bool* ok);
FaceState state_from_name(const char* s, bool* ok);
