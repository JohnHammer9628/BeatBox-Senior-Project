# ESP32-S3 + Waveshare 4.3" RGB (800×480) — Dev Board **B**

A one‑page reference for wiring, timing, PlatformIO config, libraries, controls, and recovery steps. Use this to get back to a known‑good state fast.

---

## 1) Hardware at a Glance

* **MCU:** ESP32‑S3 module on Waveshare 4.3" RGB LCD Dev Board **B**
* **Display:** 4.3" RGB TTL panel, **800×480**, driven by ESP32 parallel RGB
* **Backlight/IO:** CH422G I²C IO expander controls **Backlight Enable**
* **PSRAM/Flash (board default):** 16 MB flash + OPI PSRAM (Octal‑PSRAM)
* **Power:** USB‑C is OK for bring‑up; use a stable 5 V rail if adding power‑hungry peripherals or pushing PCLK toward ~32 MHz

> ⚠️ Note: ESP32‑S3 has **BLE only**. Consumer wireless headphones (A2DP) need **Bluetooth Classic**. For audio later, use: (a) an external BT audio transmitter, (b) an original ESP32 (Classic) as A2DP source, or (c) wired/I²S DAC during development.

---

## 2) Wiring / Pinout (Dev Board B → ESP32‑S3 GPIO)

### I²C (CH422G Backlight Expander)

* **SDA** → GPIO **8**
* **SCL** → GPIO **9**
* **Port/Bus**: I2C **0**
* **Backlight Enable**: CH422G **EXIO2** (exposed via library). We set this **HIGH** to turn BL on.

### RGB Data/Timing Lines

| Signal Group | Function             | GPIOs                     |
| ------------ | -------------------- | ------------------------- |
| Timing       | **DE, VS, HS, PCLK** | **5, 3, 46, 7**           |
| Red          | **R0..R4**           | **1, 2, 42, 41, 40**      |
| Green        | **G0..G5**           | **39, 0, 45, 48, 47, 21** |
| Blue         | **B0..B4**           | **14, 38, 18, 17, 10**    |

### Sync/TIMING Polarity & Blanking

* **hsync_pol = 0**, **vsync_pol = 0**
* Horizontal: **hfp=40, hsync=48, hbp=88** → **hTotal = 800 + 40 + 48 + 88 = 976**
* Vertical: **vfp=13, vsync=3, vbp=32** → **vTotal = 480 + 13 + 3 + 32 = 528**
* **pclk_neg = 1** (sample on falling edge)

### Pixel Clock (PCLK) Guidance

* **16 MHz** → ~**31.05 Hz** refresh (`16e6 / (976 * 528)`), rock‑solid for bring‑up
* **30–33 MHz** → ~**58–64 Hz** (closer to a 60 Hz feel). If you see tearing/flicker at high PCLK:

  * Keep ribbon/jumpers short
  * Ensure PSRAM is stable and powered
  * Try 30–31 MHz before 32–33 MHz

---

## 3) PlatformIO (Known‑Good)

```ini
[env:ws43b]
platform = espressif32@6.12.0
board = esp32-s3-devkitm-1
framework = arduino
monitor_speed = 115200

; USB CDC for Serial
build_unflags = -std=gnu++11
build_flags =
  -std=gnu++17
  -D ARDUINO_USB_MODE=1
  -D ARDUINO_USB_CDC_ON_BOOT=1
  -D BOARD_HAS_PSRAM
  -mfix-esp32-psram-cache-issue

; Waveshare module has 16MB flash & OPI PSRAM
board_build.flash_size = 16MB
board_build.psram_type = opi
board_build.arduino.memory_type = opi_opi

lib_deps =
  https://github.com/esp-arduino-libs/ESP32_IO_Expander.git#v1.1.1
  https://github.com/esp-arduino-libs/esp-lib-utils.git#v0.2.0
  moononournation/GFX Library for Arduino @ 1.5.8
```

**Why these matter**

* `BOARD_HAS_PSRAM` + `opi_opi` + cache fix → makes large framebuffers feasible and stable
* USB CDC flags → Serial over USB works from boot (helpful for logs)

---

## 4) Libraries Used

* **Arduino_GFX** (`GFX Library for Arduino` 1.5.8) — RGB panel + display object
* **ESP32_IO_Expander** 1.1.1 — CH422G I²C expander (backlight enable)
* **esp‑lib‑utils** 0.2.0 — dependency for IO Expander

---

## 5) Display Object & Timing (Arduino_GFX)

* Databus: `Arduino_ESP32RGBPanel` constructed with pins/timing above
* Display: `Arduino_RGB_Display(800, 480, panel, rotation=0, auto_flush=false)`

  * We deliberately use **`auto_flush=false`** and call `gfx->flush()` **once per frame** to reduce tearing/flicker.

---

## 6) Current App Behavior (Test UI)

* Shows **Left/Right** frequency boxes based on `baseHz ± beatHz/2`
* Header displays **Preset**, **Base**, **Beat**, and paused state
* Animated "visualizer" bar moves with beat rate
* **Session system**:

  * States: **IDLE → RUNNING → PAUSED → DONE**
  * **Countdown** (mm:ss), **progress bar**, adjustable **duration minutes**

### Serial Controls (115200)

* **Presets:** `1` Alpha, `2` Beta, `3` Theta, `4` Delta
* **Base:** `q` +5 Hz, `a` −5 Hz (min 20 Hz)
* **Beat:** `w` +0.5 Hz, `s` −0.5 Hz (clamped to preset range)
* **Reset base/beat:** `r`
* **Session:** `g` start, `p` pause/resume, `x` stop/reset, `+`/`-` minutes ±1 (1..60)

---

## 7) Bring‑Up Checklist (Fast Recovery)

1. **PSRAM Check** (Serial boot log): confirm `ESP.getPsramSize() >= 4 MB` — if not, recheck PlatformIO flags and `opi_opi` memory type
2. **Backlight**: ensure CH422G initializes on **I²C0** (SDA=8, SCL=9) and drive **EXIO2 HIGH**
3. **Display Init**: `gfx->begin()` must return true (let Arduino_GFX allocate framebuffers)
4. **PCLK**: start at **16 MHz**; after stable image, move toward **30–33 MHz** as needed
5. **Flicker**: keep `auto_flush=false`; flush once per loop; draw only dirty regions (numbers, header, progress)

---

## 8) Common Issues & Fixes

* **I²C "Invalid num" / CH422G begin() fail** → confirm I²C **pins** (8/9) and **port 0**; check pull‑ups if off‑board; ensure correct library
* **Framebuffer "no mem"** → PSRAM not enabled or wrong memory type; verify `BOARD_HAS_PSRAM`, `psram_type=opi`, `arduino.memory_type=opi_opi`, and cache fix flag
* **Octal flash/efuse warnings** → mismatched board settings; use the config above for Waveshare module defaults
* **Endless reboots** → usually memory/timing mismatch; drop PCLK to 16 MHz, confirm PSRAM, remove extra allocations
* **Flicker/tearing** → set `auto_flush=false`, single `gfx->flush()` per frame; shorten wires; try ~30–31 MHz instead of 32–33

---

## 9) Migration Notes (LVGL Later)

* Keep the **RGB panel** and **timing** the same
* Provide an **LVGL draw buffer** in PSRAM (e.g., 1–2 partial frame buffers)
* Implement `flush_cb` → push modified areas via Arduino_GFX; continue **single‑flush per frame** logic
* Mirror current state model (preset/base/beat/session) as LVGL **data model** for clean UI separation

---

## 10) Audio Transport (Planning)

* **BLE‑only on S3** → no A2DP source; options:

  1. I²S DAC → external **BT transmitter** → headphones
  2. Swap to **ESP32 (Classic)** for direct A2DP source
  3. Stay wired for lab validation
* Keep the **binaural engine math** device‑local; transport can be swapped later

---

## 11) Known‑Good Versions (for reproducibility)

* **PlatformIO platform:** `espressif32@6.12.0`
* **Arduino_GFX:** `1.5.8`
* **ESP32_IO_Expander:** `v1.1.1`
* **esp-lib-utils:** `v0.2.0`

> Save this README with your project. If the build breaks, restore these exact versions and flags first.

---

## 12) Quick "Back to Green" Steps

1. Restore **platformio.ini** above
2. Flash the last known working **`src/main.cpp`** (session UI build)
3. Open Serial @ **115200**, confirm PSRAM + "[BL] Backlight enabled"
4. Verify header renders; then press `1` and confirm Left ~195 Hz / Right ~205 Hz
5. Start a 1–2 min session with `g` and watch the countdown/progress
6. If stable, raise PCLK toward **32 MHz**, re‑test

---

*End of README*