#pragma once
#include <Arduino.h>

bool gauge_init();                              // unpack the dial picture (PSRAM)
void gauge_update(float dt);
void gauge_render_frame();
void gauge_set(float kmh, float rpm, int coolant_c);   // live values (OBD later); -1 = unknown
void gauge_set_time(int h, int m);
void gauge_demo(bool on);                       // fake drive, to see how it looks
bool gauge_is_demo();
