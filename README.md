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

- If nothing displays:
  1. Verify 3.3V and 5V rails (if used) and that BL rail is not shorted.
  2. Confirm parallel RGB wiring and that the chosen `Arduino_ESP32RGBPanel` timing parameters match the panel (hsync/vsync/pclk).
  3. Check I²C lines (SDA/SCL) for shorts to other signals.

- If touch is missing or noisy:
  1. Confirm GT911 is responding on I²C (0x5D/0x14). Use an I²C scanner or check `touch_input.cpp` debug logs.
  2. Try toggling the reset strap to change GT911 address and re-probe.
  3. Ensure correct power sequencing; some capacitive controllers behave poorly if rails are noisy during startup.

## Next steps / Roadmap

Short-term (next 1–2 sprints):

1. Display timing tuning — experiment with pixel clock and porch/scan timing in driver to reduce jitter.
2. Harden end-of-drag logic in LVGL to eliminate remaining slider snap/jump on release.
3. Add runtime orientation detection for GT911 (if possible) and a small utility in settings to flip axis without recompiling.
4. Add basic automated test harness (run in CI / local) that boots board to a known state and checks I²C device presence and LVGL init logs.

Medium-term (next month):

1. Implement a minimal settings UI to expose display timing and touch calibration to the user.
2. Improve CH422G power sequencing logic to gracefully handle brown-out or noisy rails.
3. Upstream small improvements to Arduino_GFX/driver if a timing bug is found.

Long-term / stretch:

1. Investigate DMA-assisted transfer modes or offload for larger panels if we scale beyond 480p.
2. Add a proper GT911 library dependency with full configuration support if beneficial.

## Where code lives & how to run

- Primary source: `src/` (see `main.cpp`, `touch_input.cpp`).
- Config: `platformio.ini` (per-environment build flags, orientation toggles, LVGL options).
- LVGL config overrides: `include/lv_conf.h`.

To build and upload, open the project in PlatformIO and choose the correct environment for your board. The project uses the Arduino ESP32 core and PlatformIO environments defined in `platformio.ini`.

---

If you want, I can:

- Restore any original special characters (e.g., superscript ²) everywhere if you prefer that style (I used I²C and the proper glyphs above).
- Add a short checklist and a CI job that checks for I²C device presence when a test board is available.

Completed updates: expanded Overview, Hardware, Software Stack, Current Status, Troubleshooting, and Next Steps. Preserved original addresses, GPIO pins, and version references.

## Binaural device integration

This project is intended to act as the touchscreen UI and control head for a binaural-beat generator built from analog oscillating circuits (or a hybrid analog/digital oscillator). The section below documents the intended control contract, how UI controls map to oscillator parameters, communication options between the ESP32 UI and oscillator electronics, safety considerations, and recommended next steps to implement the connection.

### Control contract (inputs / outputs)

- Inputs (from UI):
  - Preset selection (Alpha/Beta/Theta/Delta/Custom)
  - Base frequency (baseHz) — coarse and fine adjustments
  - Beat frequency (beatHz) — slider controlling the frequency difference between channels
  - Start/Stop / play state
  - Volume or amplitude control (if applicable) and mute
  - Calibration commands (e.g., zero-offset, channel trim)
- Outputs (to oscillator hardware):
  - Command packets updating oscillator parameters (baseHz, beatHz, amplitude, enable flags)
  - Preset load commands
  - Heartbeat/status queries and error responses (optional)

### UI → oscillator mapping (data shapes)

- Example JSON command (if using serial/serial-over-USB/UART or WebSocket):

  {
    "cmd": "set_params",
    "baseHz": 440.0,
    "beatHz": 5.0,
    "amplitude": 0.8,
    "enabled": true
  }

- Minimal binary packet format (UART) for small microcontrollers:
  - Header byte 0xA5
  - Command ID (1 byte)
  - Payload (N bytes)
  - CRC8 (1 byte)

  Example: [0xA5][0x01][float32 baseHz][float32 beatHz][uint8 amplitude][0xCC]

### Communication options

- UART (Serial): Simple and robust. Use a dedicated UART between the ESP32 and the oscillator controller MCU (e.g., an AVR/STM32). Pros: low overhead, easy to implement. Cons: needs a UART port and wiring.
- I²C / SPI: If the oscillator controller is another microcontroller on the same board, I²C is possible but requires master/slave setup and careful bus arbitration. SPI offers speed but requires extra lines.
- GPIO analog controls: If the oscillator circuits accept analog CV (control voltage), use DAC outputs on the ESP32 or PWM-filtered outputs to provide analog voltages proportional to baseHz/beatHz. Pros: very low-latency and simple for analog circuits. Cons: requires stable voltage scaling and filtering; accuracy depends on DAC resolution and reference.
- USB / WebSockets / BLE: For remote control or logging, the UI (ESP32) could expose settings over BLE or Wi-Fi. This is useful for remote presets or telemetry but adds complexity.

### Timing and control considerations

- Rate of updates: Oscillator hardware typically only needs parameter updates when the user stops dragging, or at a modest rate (e.g., 20–50 Hz) during drag to avoid saturating comms.
- Smooth transitions: To avoid audible artifacts, the oscillator firmware should ramp changes smoothly (exponential or linear interpolation over 20–200 ms depending on context).
- Synchronization: If multiple oscillator boards or channels are used, provide a sync or timestamp mechanism to ensure coherent updates.

### Safety and UX

- Volume/amplitude safety: Clamp amplitude controls and provide a hardware mute. Warn users in the UI if amplitude exceeds a safe threshold.
- Frequency limits: Validate baseHz and beatHz ranges before sending commands to hardware. Provide clearly labeled presets with known-safe defaults.
- Watchdog / fail-safe: The oscillator controller should implement a failsafe that mutes output if communication is lost for N seconds.

### Implementation steps (practical)

1. Decide on the transport: UART is recommended for the first pass (simple framing, robust). Define JSON or compact binary frames.
2. Implement a minimal command handler in the oscillator firmware that accepts baseHz, beatHz, amplitude, and an enable flag. Add a simple echo or ACK for development.
3. Add a comms layer in `src/` (e.g., `comms.cpp` + `comms.h`) that serializes slider/preset changes and sends updates to the oscillator board. Use debouncing/throttling during drag events.
4. Implement smooth ramping behavior in oscillator firmware to prevent clicks when parameters change.
5. Add a small settings page in the LVGL UI for mode (UART / DAC / I2C), amplitude limit, and calibration.
6. Test with a bench oscillator or a scope: send step changes and verify the analogue output follows the commanded base/beat values with expected ramping.

### Example minimal UART handshake (pseudo)

- UI sends: {"cmd":"preset","preset":"Alpha"}
- Controller replies: {"ack":"preset","preset":"Alpha","status":"ok"}

This handshake is sufficient to start iterating. For production, add CRC and versioning.

---

I will add a small `src/comms_example.cpp` stub and a sample `comms.h` if you want — which transport would you like to start with (UART JSON, compact UART binary, or DAC/CV)?
