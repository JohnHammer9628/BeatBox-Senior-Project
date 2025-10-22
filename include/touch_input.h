#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

// -------- I2C pins / speed (shared with CH422G) --------
#ifndef TOUCH_I2C_SDA
#define TOUCH_I2C_SDA 8
#endif
#ifndef TOUCH_I2C_SCL
#define TOUCH_I2C_SCL 9
#endif
#ifndef TOUCH_I2C_FREQ
#define TOUCH_I2C_FREQ 400000
#endif

// -------- Panel size for mapping --------
#ifndef TOUCH_SCREEN_W
#define TOUCH_SCREEN_W 800
#endif
#ifndef TOUCH_SCREEN_H
#define TOUCH_SCREEN_H 480
#endif

// -------- Orientation toggles --------
#ifndef TOUCH_SWAP_XY
#define TOUCH_SWAP_XY 0
#endif
#ifndef TOUCH_INVERT_X
#define TOUCH_INVERT_X 0
#endif
#ifndef TOUCH_INVERT_Y
#define TOUCH_INVERT_Y 0
#endif

// Optional hardware lines (if your board exposes them)
#ifndef TOUCH_RST_PIN
#define TOUCH_RST_PIN -1
#endif
#ifndef TOUCH_INT_PIN
#define TOUCH_INT_PIN -1
#endif

// Public API
void touch_init_and_register_lvgl();  // detect FT/GT, register LVGL indev
bool touch_present();
const char* touch_ic_name();
uint8_t touch_i2c_address();

// Utilities you can call from main for debugging
bool i2c_bus_recover(uint8_t sclPin = TOUCH_I2C_SCL, uint8_t sdaPin = TOUCH_I2C_SDA);
void i2c_full_scan_print(Stream& out = Serial);
