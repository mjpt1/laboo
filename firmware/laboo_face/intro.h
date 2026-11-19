#pragma once
#include <Arduino.h>

// Cinematic start-up animations (2-4 s, blocking). They end on the normal face.
//  1 wake up      thin slits in the dark, two blinks, eyes open
//  2 old TV       dot -> line -> orange static -> eyes come out of the static
//  3 boot ring    orange ring fills around the edge, "LABOO" glows, eyes open
//  4 comet        a glowing dot circles in, splits in two, becomes the eyes
//  5 heartbeat    a heart beats twice, then splits into two eyes
//  6 ring + wake  3 then 1
#define INTRO_COUNT 6
void intro_play(int n, uint8_t backlight);   // starts with the backlight off, ends at `backlight`
int  intro_saved();                          // the one chosen with INTRO SET n (default 6)
void intro_save(int n);
