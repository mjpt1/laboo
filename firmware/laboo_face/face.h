#pragma once
#include <Arduino.h>

enum Emotion { EMO_NEUTRAL, EMO_HAPPY, EMO_SAD, EMO_ANGRY, EMO_SURPRISED, EMO_LOVE, EMO_CONFUSED, EMO_SLEEPY, EMO_SCARED, EMO_COUNT };
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
void face_set_attention(bool on);   // someone is talking nearby: eyes open a bit and look at you
void face_poke();                    // any interaction: wakes up, resets sleep timer
void face_blink();
// desk-robot extras
void face_bounce(float px);          // dance: hop up on a beat
void face_sway(float px);            // dance: lean left (-) / right (+)
void face_set_dance(bool on);        // happy-ish eyes while music plays
void face_yawn();                    // 2.6 s yawn with an open mouth
bool face_is_yawning();
void face_caption(int textId, uint32_t ms);   // TX_* text under the eyes
void face_set_tint(int tint);        // 0 orange, 1 green
// poses on top of the face (emoji style): girl (lashes + bow), kiss ("3" mouth + heart), zip, wink, cool, dizzy, hug
enum Pose { POSE_NONE, POSE_GIRL, POSE_KISS, POSE_ZIP, POSE_WINK, POSE_COOL, POSE_DIZZY, POSE_HUG };
void face_pose(Pose p, uint32_t ms);
Pose face_get_pose();

Emotion emotion_from_name(const char* s, bool* ok);
FaceState state_from_name(const char* s, bool* ok);
