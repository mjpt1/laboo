#pragma once
#include <Arduino.h>
#include "ESP_I2S.h"

// Tiny cartoon sounds, synthesized on the fly (no audio files) and played through
// the board's ES8311 codec + speaker amp. Shares the I2S bus with the microphone.
enum Sound {
  SND_HAPPY, SND_SURPRISED, SND_ANGRY, SND_LOVE, SND_SCARED,
  SND_YAWN, SND_MORNING, SND_NIGHT, SND_WHISTLE,
  SND_DICE, SND_GO, SND_WIN, SND_LOSE, SND_POP, SND_COUNT
};

bool sound_codec_init();          // call BEFORE i2s.begin()
void sound_start(I2SClass* i2s);  // call after i2s.begin(): starts the audio task
void sound_play(Sound s);         // queue a sound (drops it if one is already waiting)
bool sound_busy();                // true while playing and a moment after (so the mic ignores it)
void sound_set_volume(int v);     // 0..100
