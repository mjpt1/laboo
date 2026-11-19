#pragma once
#include <Arduino.h>

// Persian clock screen shown while nobody is playing with Laboo.
struct ClockTime { int y, mo, d, h, mi, s; bool valid; };   // Gregorian, local (Tehran) time

void clock_set(const ClockTime& t);   // feed the RTC value (once a second is enough)
void clock_update(float dt);          // small eyes on top blink and glance
void clock_render_frame();
void clock_set_dim(bool dim);         // night: draw a bit darker

// Gregorian -> Jalali (Solar Hijri), and weekday 0 = Monday
void to_jalali(int gy, int gm, int gd, int& jy, int& jm, int& jd);
int  weekday_mon0(int y, int m, int d);
