#pragma once
// Display driver for Waveshare ESP32-S3-Touch-LCD-1.85C (ST77916, QSPI, 360x360)
// Based on the official Waveshare example, adapted to push the frame in bands
// from internal DMA RAM (fast, no LVGL needed).
#include <Arduino.h>

#define LCD_W 360
#define LCD_H 360
#define BAND_H 40                     // rows per band -> 9 bands per frame
#define LCD_BL_PIN 5

bool     display_init();
void     display_set_backlight(uint8_t percent);   // 0..100
uint16_t* display_band_buffer();                    // buffer to render the next band into
void     display_push_band(int y0, int h);          // send the band that was just rendered
void     blog(const char* fmt, ...);                 // boot log (also kept for the INFO command)
const char* blog_text();
void     display_fill(uint16_t big_endian_color);   // solid colour (test)
