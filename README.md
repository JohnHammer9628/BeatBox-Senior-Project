# ESP32-S3 4.3" RGB + GT911 (I²C) — UI Bring-Up

**Last updated:** Oct 22, 2025

**Board:** ESP32-S3 DevKitM-1 • **Display:** 800×480 RGB (parallel) • **Touch:** GT911 (I²C) • **Expander:** CH422G (I²C)  
**UI:** LVGL v9 • **Framework:** Arduino (PlatformIO)

## Overview

This repository contains bring-up and experimental code for driving a Waveshare-style 4.3" 800×480 RGB TFT from an ESP32-S3. The project demonstrates a small LVGL-based UI, capacitive touch input via a GT911 controller on I²C, and use of an on-board CH422G I²C I/O expander to manage rails (backlight, power sequencing) and extra GPIOs.

Goals for this repo:

- Produce a stable, low-shimmer LVGL UI on the RGB panel using partial updates and PSRAM-backed frame buffers.
- Get reliable capacitive touch readings from GT911 and integrate as an LVGL pointer device.
- Implement safe backlight sequencing tied to the first clean frame render to reduce boot shimmer.
- Harden slider/interaction UX so sliders don't jump or lose the user's drag.

### Key highlights

- Stable UI rendering with LVGL partial mode & PSRAM buffers
- GT911 touch working over I²C with auto-detect (FT6x36 probed/ignored if IDs don't match)
- Backlight sequencing: BL is held OFF until the first clean frame is rendered, then turned ON
- Slider UX fixes: Beat (Hz) slider knob containment and drag stabilization

## Hardware

### Components

- **MCU:** ESP32-S3 DevKitM-1
- **Display:** 800×480 RGB TFT (parallel interface) — driven with Arduino_ESP32RGBPanel / Arduino_RGB_Display
- **Touch:** Goodix GT911 capacitive controller (I²C address 0x5D or 0x14 depending on reset strap)
- **I/O Expander:** CH422G (I²C) — visible addresses on bus commonly 0x20–0x27 and 0x30–0x3F

### Wiring & notes

- I²C bus:
  - SDA = GPIO 8
  - SCL = GPIO 9
  - Start bring-up at 100 kHz, then increase to 400 kHz for normal operation
- Backlight (BL) rail is controlled through CH422G EXIO2. The logic used here holds BL LOW (off) until the firmware renders a first clean frame, then drives EXIO2 HIGH to enable the backlight — this reduces visible shimmer during boot.
- Touch controller: GT911 may appear at 0x5D or 0x14 depending on the reset/config strap. The code also probes FT6x36 at 0x38 and ignores it when IDs don't match.

### Mechanical / signal tips

- Keep RGB ribbon/cable grounds solid — jitter often correlates with grounding and cable routing.
- If you see intermittent artifacts, try lowering pixel clock or adjust timing in Arduino_ESP32RGBPanel settings.

## Software Stack

### Core components

- **Framework:** Arduino (ESP32 core) via PlatformIO (see `platformio.ini` environments)
- **Graphics:** LVGL v9.4.0 (partial updates mode used)
- **Display driver:** Arduino_GFX (Arduino_ESP32RGBPanel + Arduino_RGB_Display)

### Key files

- `src/main.cpp` — project entry, LVGL initialization, task loop
- `src/touch_input.cpp` — GT911/FT6x36 touch probing and LVGL pointer glue
- `include/lv_conf.h` — LVGL configuration overrides (PSRAM buffers, partial mode)
- `lib/ESP32_IO_Expander` (if present) — CH422G driver code and helpers

### Drivers / behavior

- Touch auto-detect: code probes FT6x36 at 0x38 first. If the chip ID doesn't match, the probe is ignored and GT911 is used when detected at 0x5D/0x14.
- If a full GT911 library isn't available, a small minimal driver in `touch_input.cpp` provides the necessary reads for LVGL pointer events.

## Current Status (detailed)

This section summarizes what we've verified on the bench and what still needs attention.

### ✅ Verified / Working

- Display initialization and LVGL UI rendering (header, preset buttons, slider, timer widgets). Partial updates with PSRAM-backed buffers are stable for normal UI tasks.
- I²C bus functional: CH422G expander and GT911 touch respond. A panel EEPROM at 0x51 is often present on the bus.
- GT911 touch reads produce sensible touch coordinates; LVGL pointer device is registered and usable.
- Backlight sequencing implemented: BL remains off until the firmware completes a clean first frame, then CH422G EXIO2 enables the backlight, which significantly reduces boot shimmer.
- Slider improvements:
  - Knob containment across presets and ranges
  - During drag, we avoid writing back to the slider immediately which prevented value-fighting and "snap-back" behavior

### ⚠️ Known issues / in-progress items

- Occasional screen shake/jitter observed (intermittent). Likely causes:
  - RGB timing/pixel clock vs. panel tolerance
  - Cable grounding/noise
  - Heavy redraw pressure during touch/drag
  Mitigations tried: reduced I²C chatter, disabled GT HUD, reduced UI updates during drag. Next: adjust RGB timing and test alternative cable/grounding.
- Rare slider release "snap-back" — main bidirectional-write problem mitigated, but a small rounding/clamping final tick may still cause a tiny jump. Next: tighten end-of-drag commit path in LVGL event handler.
- Some panels report odd GT911 orientation/config values — orientation flags live in `platformio.ini` to switch at compile time; consider making orientation runtime-configurable after validated probing.

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
