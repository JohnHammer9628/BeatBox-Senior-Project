# ESP32-S3 4.3" RGB + GT911 (I2C)  UI Bring-Up

**Last updated:** Oct 22, 2025

**Board:** ESP32-S3 DevKitM-1 | **Display:** 800x480 RGB (parallel) | **Touch:** GT911 (I2C) | **Expander:** CH422G (I2C)  
**UI:** LVGL v9 | **Framework:** Arduino (PlatformIO)

## Overview

This project drives a Waveshare-style 4.3" 800x480 RGB TFT from an ESP32-S3, renders a simple LVGL UI, and reads capacitive touch via a GT911 controller on I2C. An on-board CH422G I/O expander shares the I2C bus and is used to manage rails (notably the backlight).

### Key Features

- Stable UI rendering with LVGL partial mode & PSRAM buffers
- GT911 touch working over I2C with auto-detect (FT6x36 probed/ignored if IDs do not match)
- Backlight sequencing: BL is held OFF until first clean frame is rendered, then turned ON (reduces shimmer at boot)
- Slider UX fixes: The Beat (Hz) slider knob always remains fully on-screen for all presets and ranges

## Hardware

### Components
- **MCU:** ESP32-S3 DevKitM-1
- **Display:** 800x480 RGB TFT, parallel Arduino_ESP32RGBPanel
- **Touch:** GT911 (I2C @ 0x5D or 0x14 depending on reset strap)
- **I/O Expander:** CH422G (addresses 0x20-0x27 and 0x30-0x3F visible)

### I2C Configuration
- **SDA:** GPIO 8
- **SCL:** GPIO 9
- **Speed:** start @ 100 kHz for bring-up, then bump to 400 kHz
- **BL Control:** CH422G EXIO2 (held LOW until first clean frame, then HIGH)

## Software Stack

### Core Components
- **Framework:** Arduino (ESP32 core) via PlatformIO
- **Graphics:** LVGL v9.4.0
- **Display driver:** Arduino_GFX (Arduino_ESP32RGBPanel + Arduino_RGB_Display)

### Drivers
- **Touch:** touch_input.cpp
  - Auto-detect FT6x36 @ 0x38 (ignored if IDs look wrong)
  - GT911 @ 0x5D or 0x14 (raw minimal driver used if GT911 lib absent)
- **I/O expander:** ESP32_IO_Expander (CH422G)

## Current Status

### What Works
- **Display init and LVGL UI:** header, preset buttons (Alpha/Beta/Theta/Delta), slider, timer widgets
- **I2C shared bus:** CH422G + GT911 + panel EEPROM @ 0x51 often present
- **Touch:** GT911 reads are valid, LVGL pointer device registered
- **Backlight sequencing:** BL turns on after first frame - reduces initial shimmer
- **Slider improvements:**
  - Knob always fully visible and usable across all presets/ranges
  - No value fighting during drag - prevents "snap back to start"

### Known Issues & Observations

#### 1. Occasional Screen Shake/Jitter
- Still appears intermittently, especially while touching/dragging
- Likely causes: RGB timing, cable/grounding, and redraw pressure
- Mitigations:
  - Reduced I2C chatter (disabled GT HUD)
  - Kept UI updates light during drag
  - Next focus: further display timing tuning

#### 2. Slider Release "Snap-back" (rare)
- Main culprit (bidirectional write during drag) mitigated
- Remaining jumps likely due to:
  - LVGL clamping to range
  - Final compute/rounding tick
- Plan: Tighten end-of-drag path next

#### 3. Orientation/Coordinate Sanity
- Some panels report quirky GT911 config values
- Orientation flags are compile-time toggles in platformio.ini

## UX & Math (Binaural Beat Mapping)

### Current Behavior (Intentional)
- Left = baseHz - beatHz/2
- Right = baseHz + beatHz/2

Increasing Beat (Hz) widens the split around the base, so left goes down and right goes up. Alternative approach: anchor one ear to baseHz instead.
