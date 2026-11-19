#pragma once
#include <Arduino.h>

// Turn-by-turn from the phone (web page in Chrome) over BLE.
// Message (UTF-8 text written to the RX characteristic):
//   "N,<turn>,<metres>,<exit>"   next manoeuvre
//   "X"                          navigation ended
// turn: S straight, SL/SR slight, L/R, HL/HR sharp, U u-turn, O roundabout, A arrive
enum NavTurn { NT_STRAIGHT, NT_SLIGHT_L, NT_SLIGHT_R, NT_LEFT, NT_RIGHT, NT_SHARP_L, NT_SHARP_R,
               NT_UTURN, NT_ROUND, NT_ARRIVE };

void nav_begin();                     // start BLE advertising as "Laboo"
bool nav_active();                    // a route is running (message in the last 15 s)
bool nav_connected();
void nav_handle(const char* msg);     // also used by the serial NAV command
void nav_render_frame(float kmh, bool kmhValid);
int  nav_metres();
bool nav_take_time(int v[6]);         // phone sent "T,y,mo,d,h,mi,s" (local time)
NavTurn nav_turn();
