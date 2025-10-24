NestGuard  Smart Baby Monitor (ESP32-S3 UI + Raspberry Pi Nursery Node)

Team: John Hammer, Sam Hammer, Jasen Pritchard
Instructor: Professor Levine
Date: 10/23/2025

## Overview

NestGuard is a two-part system:

- Nursery node (Raspberry Pi): camera motion sensing, audio input/output, optional presence sensing, and MQTT publishing.
- Parents console (ESP32-S3 + 4.3" RGB TFT + GT911 touch): bedside touchscreen that displays live state (cry likelihood, motion recency, sound level, now playing) and sends commands (play/stop/volume/threshold) to the nursery node.

This repository contains the ESP32-S3 UI firmware. The Pi service (camera/audio/MQTT) is described in the "Raspberry Pi service" section below.

> Not a medical device. This project is for educational purposes only.

---

## What's working now (ESP32-S3)

- Stable LVGL 9 UI at 800×480, 16-bit color (partial render buffers in PSRAM)
- GT911 capacitive touch over I²C with auto-detect and calibration offsets
- I²C bus shared with CH422G I/O expander
- On-screen diagnostics (live I²C scan box; serial logs)
- UI controls for Play / Stop and a Volume slider (simulated until MQTT hookup)
- Cry badge, sound bar, motion line, now-playing label (driven by a local simulator)
- Reduced screen jitter by deferring heavy UI writes during slider drag and tuning RGB timings
- Display centering via corrected RGB porches (no software X offset)

---

## System architecture

- Transport: WiFi + MQTT (Mosquitto on the Pi)
- Payloads: Small JSON messages (telemetry & commands)
- Video: RTSP from the Pi (ESP shows status/snapshots only; phones/laptops view the stream)

### Topics

- Telemetry (Pi  ESP, retained): `nestguard/telemetry/state`
- Commands (ESP  Pi, QoS 1):
  - `nestguard/cmd/play`
  - `nestguard/cmd/stop`
  - `nestguard/cmd/set_volume`
  - `nestguard/cmd/set_thresholds`
  - `nestguard/cmd/set_auto_soothe`

### Example state payload

```json
{
  "ts": 1698112345,
  "crying": false,
  "sound_level": 27,
  "motion": "idle",
  "temp_c": 22.6,
  "now_playing": "none",
  "volume": 40,
  "auto_soothe": true,
  "camera": { "rtsp": "rtsp://pi.local:8554/baby" }
}
```

---

## Hardware

### Parents console (this repo)

- ESP32-S3 DevKitM-1 (QFN module; OPI PSRAM)
- 4.3" 800×480 RGB TFT panel (Waveshare-style RGB TTL)
- GT911 capacitive touch (I²C @ 0x5D or 0x14)
- CH422G I/O expander (IC; drives rails/backlight enable)

### Nursery node (separate setup)

- Raspberry Pi 4B (or 3B+)
- Camera: Raspberry Pi Camera Module 3 NoIR Wide (+ 940 nm IR illuminator)
- Audio in: USB microphone
- Audio out: USB DAC + powered speaker (or USB speaker bar)
- (Optional) LD2410 mmWave presence sensor (UART)

### Wiring (ESP32-S3 side)

- I²C: `SDA = GPIO 8`, `SCL = GPIO 9` (100 kHz at bring-up, 400 kHz after detect)
- RGB panel pins: see `src/main.cpp` (Arduino_ESP32RGBPanel wiring table)
- Backlight rail: controlled via CH422G EXIO2 (set HIGH after first clean frame)

---

## Firmware (ESP32-S3)

### Toolchain

- PlatformIO (espressif32@6.12.0)
- Arduino framework (ESP32 core)

### Libraries

- LVGL 9.4.0
- GFX Library for Arduino 1.5.8
- ESP32_IO_Expander v1.1.1
- esp-lib-utils v0.2.0
- Adafruit FT6206 (for FT6x36 fallback probes)

### Build snippet (already configured in `platformio.ini`)

```ini
board = esp32-s3-devkitm-1
board_build.flash_size = 16MB
board_build.psram_type = opi
board_build.arduino.memory_type = qio_opi

build_flags =
  -std=gnu++17
  -D LV_CONF_INCLUDE_SIMPLE=1
  -D LV_CONF_PATH="lv_conf.h"
  -D BOARD_HAS_PSRAM
  -mfix-esp32-psram-cache-issue
  -D TOUCH_I2C_FREQ=100000
  ; Orientation (adjust if needed)
  -D TOUCH_SWAP_XY=0
  -D TOUCH_INVERT_X=0
  -D TOUCH_INVERT_Y=0
```

### Display timing (centering fix)

We found the panels active area was shifted right; centered using these constructor args in `main.cpp` (Arduino_ESP32RGBPanel):

- HFP = 108, HSYNC = 48, HBP = 20
- VFP = 13, VSYNC = 3, VBP = 32
- PCLK = 16 MHz, pclk_neg = 1

If your panel batch varies, adjust HFP/HBP by 812 pixels to center (keep HSYNC = 48).

### Touch calibration

- Orientation toggles via `platformio.ini`: `TOUCH_SWAP_XY`, `TOUCH_INVERT_X`, `TOUCH_INVERT_Y`
- Fine offsets in `include/touch_input.h`:

```c
#define TOUCH_X_OFFSET 0
#define TOUCH_Y_OFFSET 10 // positive pushes touch DOWN
```

If taps land high/low, bump `TOUCH_Y_OFFSET` by 2 until buttons feel right.

---

## Key files

- `src/main.cpp`  Baby Monitor UI (cry badge, sound bar, motion line, now-playing, play/stop/volume)
- `src/touch_input.cpp`  Touch auto-detect (FT6x36/GT911), robust GT911 raw reader, LVGL indev
- `include/touch_input.h`  Public touch API + I²C/geometry/offsets
- `include/lv_conf.h`  LVGL config (800×480, 16-bit, Montserrat fonts)

---

## UI map (ESP32-S3)

- Top bar: Baby Monitor  Local Status
- Left column: I²C scan box (live device addresses)
- Top-right badge: CRY LIKELY (red) or Calm (green)
- Sound level: label + bar (0–100)
- Motion line: Motion: detected/idle  Last movement: Ns ago
- Now Playing: current track + Volume slider (0–100)
- Buttons: Play, Stop

Serial shortcuts (dev): `p`=Play, `x`=Stop, `+`/`-` = volume ±5, `w`/`s` = cry threshold ±2

Until MQTT is wired, the UI runs a local simulator (randomized sound level, debounce window for cry). Widgets and event handlers are ready and will bind to MQTT next.

---

## Raspberry Pi service (next task)

Create a small Python daemon (systemd service) at `/opt/nestguard` with these responsibilities:

- Camera RTSP: `libcamera-vid`  `rtsp-simple-server`
- Motion detection: OpenCV on downscaled grayscale frames (e.g., 640×360 @ 10–15 FPS) using frame differencing or MOG2; publish `moving`/`idle`
- Audio in: `sounddevice` (PortAudio) to compute short-term RMS (2030 ms frames)  `cry likely` when RMS > threshold for >600 ms
- Audio out: `mpg123`/`aplay` for lullabies/white noise; volume controlled via `amixer`
- MQTT: `paho-mqtt` to publish telemetry and subscribe to `nestguard/cmd/*`

Publish retained `nestguard/telemetry/state` every 300–500 ms (or on change). Subscribe to `nestguard/cmd/*`, apply immediately, then echo new state in telemetry.

---

## Security quick wins

- Mosquitto with username/password; `allow_anonymous = false`
- LAN-only (no port-forward); WPA2 WiFi
- Volume cap at both ends (firmware clamps 0–70%)

---

## Roadmap

- ESP32: add WiFi + MQTT client (subscribe to `telemetry/state`, publish `cmd/*`)
- ESP32: show RTSP URL / QR for opening the video on a phone
- Pi: Python service (audio RMS  cry flag; OpenCV motion; audio playback; MQTT I/O)
- Pi: configuration file (thresholds, volumes, auto-soothe duration/cooldown) + persistence
- Optional: LD2410 mmWave presence to complement camera
- Optional: BLE pairing fallback if WiFi is down (ESP screens broker IP/creds)
- Optional: snapshot push (Pi publishes a JPEG on motion/cry to an HTTP endpoint viewable from a phone)

---

## Troubleshooting

### Screen shimmer / jitter

- Keep RGB ribbon short; add a ground strap between panel and ESP board
- PCLK 16 MHz is stable; try 12–18 MHz and small porch changes if needed
- Avoid heavy redraws during drag (already handled in code)

### UI shifted / black bands

- Tweak HFP/HBP a few pixels (e.g., HFP 100–116; HBP 16–28)

### Touch misses / must press above

- Increase `TOUCH_Y_OFFSET` (e.g., 10 → 12 or 14)
- Verify `TOUCH_SWAP_XY` & invert flags match the glass orientation

### GT911 not detected

- Ensure CH422G brings rails high; code tries both INT/RST EXIO mappings (7/6 and 6/7)
- Bus recovery runs at boot; check I²C scan box for `0x5D` or `0x14`

---

## Ethics & safety

- No cords in the crib; keep devices out of babys reach
- Safe audio levels (firmware cap; use a quiet speaker)
- Privacy: local-only broker, no cloud by default

> Not a medical device.

---

## License

Educational use; see course/project guidelines. Add an explicit license if you open-source the Pi service.

---

## Acknowledgments

- LVGL, Arduino-ESP32, GFX Library, ESP32_IO_Expander
- Class resources and prior bring-up work from the original binaural-beats UI







