#pragma once
// Wi-Fi OBD-II reader (ELM327 Wi-Fi adapters, e.g. "WiFi_OBDII" at 192.168.0.10:35000).
// Runs in its own task: joins the adapter's Wi-Fi, sets up the ELM327 and polls live values.
#include <Arduino.h>
#include <time.h>

struct ObdData {
  bool  wifi;        // joined the adapter's Wi-Fi
  bool  link;        // ELM327 answered and the car's ECU replied
  float kmh;         // -1 = unknown
  float rpm;         // -1 = unknown
  int   coolant;     // deg C, -1000 = unknown
  float volts;       // battery, -1 = unknown
  int   dtcCount;    // stored fault codes, -1 = unknown
  bool  checkEngine; // MIL lamp on
  char  dtc[8][6];   // first fault codes, e.g. "P0301"
  uint32_t lastData; // millis() of the last good reading
  float fuelPct;     // tank level, -1 = the car does not report it
  float lph;         // fuel flow now, litres/hour (from air flow), -1 = unknown
  float tripKm, tripL, maxKmh;   // since the board was powered (the car's USB = this drive)
  uint32_t tripSec;  // seconds with the engine data live
};

void obd_begin();                         // starts the task (reads SSID/IP from settings)
const ObdData& obd_data();
bool obd_live();                          // data arrived in the last 3 s
void obd_set_wifi(const char* ssid, const char* pass, const char* ip, int port);   // saved in flash
void obd_status(Print& out);              // one-line status for the USB console
void obd_clear_codes();
void obd_set_home_wifi(const char* ssid, const char* pass);   // saved; used for the clock (NTP)
void obd_panel(uint32_t minutes);    // Wi-Fi "Laboo" + settings page for a while (sets the clock from the browser)
bool obd_panel_on();
bool obd_take_time(struct tm* t);     // true once after the internet clock was read                   // asks the car to clear fault codes (Mode 04)
