// src/main.cpp
/**
 * WS43B (ESP32-S3 + CH422G + GT911) — LVGL UI + safer GT911 reset
 * - Only EXIO2 is forced HIGH (backlight); others left INPUT (high-Z)
 * - Try GT911 reset via EXIO6/7 (both A/B mappings)
 * - After reset: INT -> INPUT (release), RST -> OUTPUT HIGH (hold)
 * - Touch driver is in touch_input.cpp (auto-detect + raw GT reader)
 * - Adds periodic GT status log for quick debugging
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <esp_io_expander.hpp>
using namespace esp_expander;

#include <databus/Arduino_ESP32RGBPanel.h>
#include <display/Arduino_RGB_Display.h>

#include <lvgl.h>
#include "touch_input.h"   // your auto-detect + raw GT reader

/* ------------------------- Pins / I2C ------------------------- */
static constexpr int I2C_SDA  = 8;
static constexpr int I2C_SCL  = 9;

/* ------------------------- CH422G ----------------------------- */
static constexpr int I2C_PORT = 0;
static CH422G *exio = nullptr;
static constexpr int EXIO_BL = 2;   // backlight rail

/* ------------------------ Display HW -------------------------- */
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  /* DE,VS,HS,PCLK */ 5, 3, 46, 7,
  /* R0..R4 */ 1, 2, 42, 41, 40,
  /* G0..G5 */ 39, 0, 45, 48, 47, 21,
  /* B0..B4 */ 14, 38, 18, 17, 10,
  /* hsync_pol */ 0, /* hfp,hsync,hbp */ 40, 48, 88,
  /* vsync_pol */ 0, /* vfp,vsync,vbp */ 13, 3, 32,
  /* pclk_neg */ 1,  /* prefer speed */ 16000000
);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(800, 480, rgbpanel, 0, false);

/* ------------------------------ App Model ------------------------------ */
struct Preset { const char* name; float baseHz, beatHz, beatMin, beatMax; };
static Preset PRESETS[] = {
  {"Alpha", 200.0f, 10.0f,  8.0f, 12.0f},
  {"Beta",  220.0f, 18.0f, 13.0f, 30.0f},
  {"Theta", 180.0f,  6.0f,  4.0f,  7.0f},
  {"Delta", 150.0f,  2.0f,  0.5f,  3.0f}
};
enum class SessionState : uint8_t { IDLE, RUNNING, PAUSED, DONE };

static int   presetIdx = 0;
static float baseHz    = PRESETS[0].baseHz;
static float beatHz    = PRESETS[0].beatHz;
static float fLeft     = 0.0f, fRight = 0.0f;

static SessionState state = SessionState::IDLE;
static uint8_t  sessionMinutes = 10;
static uint32_t sessionStartMs = 0, accumulatedMs = 0;

/* ----------------------------- LVGL glue ------------------------------ */
static lv_display_t* disp;
static lv_obj_t*  headerLabel;
static lv_obj_t*  lrLabel;
static lv_obj_t*  beatSlider;
static lv_obj_t*  beatValueLabel;
static lv_obj_t*  presetBtns[4];
static lv_obj_t*  startBtn;
static lv_obj_t*  pauseBtn;
static lv_obj_t*  stopBtn;
static lv_obj_t*  minutesSB;
static lv_obj_t*  minutesMinusBtn;
static lv_obj_t*  minutesPlusBtn;
static lv_obj_t*  timeLeftLabel;
static lv_obj_t*  progressBar;

/* --- On-screen diagnostics --- */
static lv_obj_t* diagLabel = nullptr;     // general status (touch, etc.)
static lv_obj_t* scanBox = nullptr;       // multi-line I2C scan box
static void diag_set(const char* s) { if (diagLabel) lv_label_set_text(diagLabel, s); }
static void scan_set(const char* s) { if (scanBox) lv_label_set_text(scanBox, s); }
static void i2c_scan_multiline(const char* tag);

/* LVGL buffers */
static constexpr int LV_BUF_LINES = 40;
static lv_color_t* lv_buf1 = nullptr;
static lv_color_t* lv_buf2 = nullptr;

/* ----------------------------- Utilities ------------------------------ */
static inline void computeEngine() {
  const Preset& p = PRESETS[presetIdx];
  if (beatHz < p.beatMin) beatHz = p.beatMin;
  if (beatHz > p.beatMax) beatHz = p.beatMax;
  fLeft  = baseHz - (beatHz * 0.5f);
  fRight = baseHz + (beatHz * 0.5f);
}
static inline uint32_t sessionElapsedMs() {
  if (state == SessionState::RUNNING) return accumulatedMs + (millis() - sessionStartMs);
  return accumulatedMs;
}
static inline uint32_t sessionTotalMs() { return (uint32_t)sessionMinutes * 60u * 1000u; }
static void print_mem(const char* tag) {
  Serial.printf("[%s] Heap free:%u KB  PSRAM:%u/%u KB\n",
    tag, (uint32_t)(ESP.getFreeHeap()/1024),
    (uint32_t)(ESP.getFreePsram()/1024), (uint32_t)(ESP.getPsramSize()/1024));
}

/* ---------------------------- LVGL Styles ----------------------------- */
static lv_style_t style_bg, style_text_small, style_text_large, style_btn, style_scan;

static void init_styles() {
  lv_style_init(&style_bg);
  lv_style_set_bg_color(&style_bg, lv_color_hex(0x000000));
  lv_style_set_text_color(&style_bg, lv_color_hex(0xFFFFFF));
  lv_obj_add_style(lv_screen_active(), &style_bg, 0);

  lv_style_init(&style_text_small);
  lv_style_set_text_color(&style_text_small, lv_color_hex(0xFFFFFF));
  lv_style_set_text_font(&style_text_small, &lv_font_montserrat_20);

  lv_style_init(&style_text_large);
  lv_style_set_text_color(&style_text_large, lv_color_hex(0xFFFFFF));
  lv_style_set_text_font(&style_text_large, &lv_font_montserrat_24);

  lv_style_init(&style_btn);
  lv_style_set_bg_color(&style_btn, lv_color_hex(0x303030));
  lv_style_set_radius(&style_btn, 10);
  lv_style_set_pad_all(&style_btn, 8);

  // Scan box style (yellow text)
  lv_style_init(&style_scan);
  lv_style_set_text_color(&style_scan, lv_color_hex(0xFFFF00));
  lv_style_set_text_font(&style_scan, &lv_font_montserrat_20);
}

/* ---------------------------- LVGL UI bits ---------------------------- */
static void ui_update_header() {
  char hdr[128];
  snprintf(hdr, sizeof(hdr), "Preset: %s   Base: %.1f Hz   Beat: %.2f Hz",
           PRESETS[presetIdx].name, baseHz, beatHz);
  lv_label_set_text(headerLabel, hdr);
}
static void ui_update_lr() {
  char lr[96];
  snprintf(lr, sizeof(lr), "Left: %.2f Hz    Right: %.2f Hz", fLeft, fRight);
  lv_label_set_text(lrLabel, lr);
}
static void ui_update_slider() {
  if (!beatSlider) return;
  lv_slider_set_value(beatSlider, (int32_t)lrintf(beatHz * 100.0f), LV_ANIM_OFF);
  char bv[32]; snprintf(bv, sizeof(bv), "%.2f Hz", beatHz);
  lv_label_set_text(beatValueLabel, bv);
}
static void ui_update_minutes() {
  if (minutesSB) lv_spinbox_set_value(minutesSB, sessionMinutes);
}
static void ui_update_progress() {
  if (!progressBar || !timeLeftLabel) return;
  uint32_t total = sessionTotalMs();
  uint32_t elapsed = sessionElapsedMs();
  if (state == SessionState::RUNNING && elapsed >= total && total>0) state = SessionState::DONE;

  lv_bar_set_range(progressBar, 0, (int32_t)total);
  lv_bar_set_value(progressBar, (int32_t)min(elapsed, total), LV_ANIM_OFF);

  uint32_t remain = (elapsed >= total) ? 0 : (total - elapsed);
  uint32_t mm = remain / 60000u, ss = (remain % 60000u) / 1000u;
  const char* sname = (state==SessionState::IDLE)?"IDLE":(state==SessionState::RUNNING)?"RUNNING":(state==SessionState::PAUSED)?"PAUSED":"DONE";
  char tl[120];
  snprintf(tl, sizeof(tl), "Session: %s   Duration: %u min   Time Left: %02u:%02u",
           sname, (unsigned)sessionMinutes, (unsigned)mm, (unsigned)ss);
  lv_label_set_text(timeLeftLabel, tl);
}
static void ui_sync_all() { computeEngine(); ui_update_header(); ui_update_lr(); ui_update_slider(); ui_update_minutes(); ui_update_progress(); }

/* ------------------------------ Events ------------------------------- */
static void beat_slider_event_cb(lv_event_t* e) {
  int32_t v = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
  beatHz = v / 100.0f; ui_sync_all();
}
static void preset_btn_event_cb(lv_event_t* e) {
  uintptr_t idx = (uintptr_t) lv_event_get_user_data(e);
  presetIdx = (int)idx; baseHz = PRESETS[presetIdx].baseHz; beatHz = PRESETS[presetIdx].beatHz; ui_sync_all();
}
static void startSession(){ if (state!=SessionState::RUNNING){ if(state==SessionState::DONE) accumulatedMs=0; state=SessionState::RUNNING; sessionStartMs=millis(); } }
static void pauseSessionToggle(){ if(state==SessionState::RUNNING){ accumulatedMs+= (millis()-sessionStartMs); state=SessionState::PAUSED; } else if(state==SessionState::PAUSED){ sessionStartMs=millis(); state=SessionState::RUNNING; } }
static void stopSession(){ state=SessionState::IDLE; accumulatedMs=0; }
static void start_btn_event_cb(lv_event_t*) { startSession(); ui_update_progress(); }
static void pause_btn_event_cb(lv_event_t*) { pauseSessionToggle(); ui_update_progress(); }
static void stop_btn_event_cb (lv_event_t*) { stopSession(); ui_update_progress(); }
static void minutes_minus_event_cb(lv_event_t*) { if (sessionMinutes>1)  sessionMinutes--; ui_update_minutes(); ui_update_progress(); }
static void minutes_plus_event_cb (lv_event_t*) { if (sessionMinutes<60) sessionMinutes++; ui_update_minutes(); ui_update_progress(); }

/* ------------------------------ Build UI ------------------------------ */
static lv_obj_t* make_btn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb, void* ud=nullptr, int w=120, int h=44) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_add_style(btn, &style_btn, 0);
  lv_obj_set_size(btn, w, h);
  if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t* lb = lv_label_create(btn);
  lv_obj_add_style(lb, &style_text_small, 0);
  lv_label_set_text(lb, txt);
  lv_obj_center(lb);
  return btn;
}

static void build_ui() {
  init_styles();

  headerLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(headerLabel, &style_text_large, 0);
  lv_obj_set_pos(headerLabel, 12, 10);

  lrLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(lrLabel, &style_text_large, 0);
  lv_obj_set_pos(lrLabel, 12, 48);

  const int presY = 92, presX0 = 12, presW = 150, presH = 50, presGap = 10;
  for (int i = 0; i < 4; ++i) {
    presetBtns[i] = make_btn(lv_screen_active(), PRESETS[i].name, preset_btn_event_cb, (void*)(uintptr_t)i, presW, presH);
    lv_obj_set_pos(presetBtns[i], presX0 + i*(presW + presGap), presY);
  }

  lv_obj_t* beatLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(beatLabel, &style_text_large, 0);
  lv_label_set_text(beatLabel, "Beat (Hz):");
  lv_obj_set_pos(beatLabel, 12, 158);

  beatSlider = lv_slider_create(lv_screen_active());
  lv_obj_set_size(beatSlider, 600, 26);
  lv_obj_set_pos(beatSlider, 12, 192);
  lv_slider_set_range(beatSlider, 400, 3000); // 4.00..30.00
  lv_slider_set_value(beatSlider, (int32_t)lrintf(beatHz * 100.0f), LV_ANIM_OFF);
  lv_obj_add_event_cb(beatSlider, beat_slider_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

  beatValueLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(beatValueLabel, &style_text_large, 0);
  lv_obj_set_style_text_color(beatValueLabel, lv_color_hex(0xFFFF00), 0);
  lv_obj_set_pos(beatValueLabel, 620, 188);

  lv_obj_t* durLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(durLabel, &style_text_large, 0);
  lv_label_set_text(durLabel, "Duration (min):");
  lv_obj_set_pos(durLabel, 12, 232);

  minutesSB = lv_spinbox_create(lv_screen_active());
  lv_spinbox_set_range(minutesSB, 1, 60);
  lv_spinbox_set_value(minutesSB, sessionMinutes);
  lv_spinbox_set_rollover(minutesSB, false);
  lv_obj_set_size(minutesSB, 100, 48);
  lv_obj_set_pos(minutesSB, 12, 268);
  lv_obj_add_style(minutesSB, &style_text_large, 0);

  minutesMinusBtn = make_btn(lv_screen_active(), "−", minutes_minus_event_cb, nullptr, 48, 48);
  lv_obj_set_pos(minutesMinusBtn, 120, 268);

  minutesPlusBtn = make_btn(lv_screen_active(), "+", minutes_plus_event_cb, nullptr, 48, 48);
  lv_obj_set_pos(minutesPlusBtn, 172, 268);

  startBtn = make_btn(lv_screen_active(), "Start", start_btn_event_cb, nullptr, 150, 50);
  pauseBtn = make_btn(lv_screen_active(), "Pause/Resume", pause_btn_event_cb, nullptr, 190, 50);
  stopBtn  = make_btn(lv_screen_active(), "Stop",  stop_btn_event_cb,  nullptr, 150, 50);
  lv_obj_set_pos(startBtn, 250, 264);
  lv_obj_set_pos(pauseBtn, 410, 264);
  lv_obj_set_pos(stopBtn,  610, 264);

  timeLeftLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(timeLeftLabel, &style_text_large, 0);
  lv_obj_set_pos(timeLeftLabel, 12, 326);

  progressBar = lv_bar_create(lv_screen_active());
  lv_obj_set_size(progressBar, 776, 24);
  lv_obj_set_pos(progressBar, 12, 360);

  // Multi-line scan box (top area)
  scanBox = lv_label_create(lv_screen_active());
  lv_obj_add_style(scanBox, &style_scan, 0);
  lv_obj_set_width(scanBox, 776);
  lv_label_set_long_mode(scanBox, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(scanBox, 12, 12 + 24 + 6);

  // General diag (bottom-left)
  diagLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(diagLabel, &style_text_small, 0);
  lv_obj_set_pos(diagLabel, 12, 400);
  lv_label_set_text(diagLabel, "diag: ready");

  // Initial sync
  ui_sync_all();
}

/* ------------------------------- Serial ------------------------------- */
static void applyPreset(int idx) { presetIdx = idx; baseHz = PRESETS[idx].baseHz; beatHz = PRESETS[idx].beatHz; ui_sync_all(); }
static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case '1': applyPreset(0); break; case '2': applyPreset(1); break;
      case '3': applyPreset(2); break; case '4': applyPreset(3); break;
      case 'q': baseHz += 5.0f; break; case 'a': baseHz -= 5.0f; if (baseHz<20.0f) baseHz=20.0f; break;
      case 'w': beatHz += 0.5f; break; case 's': beatHz -= 0.5f; break;
      case 'r': baseHz = PRESETS[presetIdx].baseHz; beatHz = PRESETS[presetIdx].beatHz; break;
      case '+': if (sessionMinutes<60) sessionMinutes++; break; case '-': if (sessionMinutes>1) sessionMinutes--; break;
      default: break;
    }
    ui_sync_all();
  }
}

/* -------- I2C scan helper: multi-line -------- */
static void i2c_scan_multiline(const char* tag) {
  String out;
  out.reserve(256);
  out += "I2C ";
  out += (tag ? tag : "");
  out += ":\n";

  int col = 0;
  for (uint8_t a = 1; a < 127; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      char buf[8];
      snprintf(buf, sizeof(buf), "0x%02X ", a);
      out += buf;
      col++;
      if (col >= 12) { out += "\n"; col = 0; }
    }
  }
  if (col == 0 && out.indexOf('\n') == (int)out.length()-1) {
    out += "(none)";
  }

  Serial.println(out);
  scan_set(out.c_str());
}

/* ---------------------- GT911 reset via CH422G ------------------------ */
/* Try two likely mappings:
 *   Mapping A: INT=EXIO7, RST=EXIO6
 *   Mapping B: INT=EXIO6, RST=EXIO7
 * After reset: INT->INPUT (release), RST->OUTPUT HIGH (hold)
 */
static bool gt_reset_seq(int exio_int, int exio_rst) {
  if (!exio) return false;

  // Prepare: make both outputs, default HIGH
  exio->pinMode(exio_int, OUTPUT);
  exio->pinMode(exio_rst, OUTPUT);
  exio->digitalWrite(exio_int, HIGH);
  exio->digitalWrite(exio_rst, HIGH);
  delay(2);

  // 1) INT -> LOW
  exio->digitalWrite(exio_int, LOW);
  delay(1);

  // 2) RST low -> high
  exio->digitalWrite(exio_rst, LOW);  delay(10);
  exio->digitalWrite(exio_rst, HIGH); delay(10);

  // 3) Release INT (set to INPUT/high-Z), keep RST high
  exio->pinMode(exio_int, INPUT);
  exio->digitalWrite(exio_rst, HIGH); // keep as output high

  delay(20);

  // Probe
  Wire.beginTransmission(0x5D);
  bool ok = (Wire.endTransmission() == 0);
  Serial.printf("[*] GT911 reset via CH422G: INT=EXIO%d RST=EXIO%d -> %s\n",
                exio_int, exio_rst, ok ? "0x5D ACK" : "NO ACK");
  return ok;
}

static bool try_gt_reset() {
  // Mapping A
  if (gt_reset_seq(7, 6)) { Serial.println("[*] Mapping A OK (INT=EXIO7, RST=EXIO6)"); return true; }
  // Mapping B
  if (gt_reset_seq(6, 7)) { Serial.println("[*] Mapping B OK (INT=EXIO6, RST=EXIO7)"); return true; }
  Serial.println("[*] No ACK after A/B reset (will continue anyway)");
  return false;
}

/* ------------------ GT status poll (debug HUD) ------------------------ */
static void gt_status_timer_cb(lv_timer_t*) {
  // Read GT911 status (0x814E) just for HUD; leaves real reading to touch_input.cpp
  const uint8_t addr = 0x5D;
  uint8_t reg[2] = { 0x81, 0x4E };
  uint8_t status = 0x00;

  Wire.beginTransmission(addr);
  Wire.write(reg, 2);
  if (Wire.endTransmission(false) == 0 && Wire.requestFrom((int)addr, 1) == 1) {
    status = Wire.read();
    uint8_t n = status & 0x0F;
    bool buf_ready = status & 0x80;
    Serial.printf("GT HUD: status=0x%02X n=%u %s\n", status, n, buf_ready ? "[buf]" : "");
  }
}

/* ------------------------ Timers / Lifecycle -------------------------- */
static void session_timer_cb(lv_timer_t*) { ui_update_progress(); }

/* -------------------------------- setup -------------------------------- */
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) { }
  delay(150);

  if (ESP.getPsramSize() < 4*1024*1024) { Serial.println("[fatal] No PSRAM"); for(;;) delay(1000); }

  // --- I2C bring-up (slow first) ---
  Wire.end();
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delay(2);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  delay(3);
  (void)i2c_bus_recover(); // from touch_input.cpp (safe)

  // --- CH422G: only BL high, others INPUT (do NOT force all high) ---
  exio = new CH422G(I2C_PORT, I2C_SDA, I2C_SCL);
  if (exio->begin()) {
    // Backlight rail ON
    exio->pinMode(EXIO_BL, OUTPUT);
    exio->digitalWrite(EXIO_BL, HIGH);
    Serial.println("[exio] EXIO2 -> HIGH (BL rail)");

    // Leave other pins high-Z unless we actively use them
    for (int pin = 0; pin < 8; ++pin) {
      if (pin == EXIO_BL) continue;
      exio->pinMode(pin, INPUT);
    }
    Serial.println("[exio] EXIO[others] -> INPUT (released)");

    // Try GT reset via EXIO pairs
    try_gt_reset();
  } else {
    Serial.println("[exio] CH422G begin() failed");
  }
  delay(40);

  // Speed up I2C after setup/detect
  Wire.setClock(400000);

  // --- Display + LVGL ---
  if (!gfx->begin()) { Serial.println("[fatal] gfx->begin() failed"); for(;;) delay(1000); }

  lv_init();

  const size_t buf_pixels = 800 * LV_BUF_LINES;
  lv_buf1 = (lv_color_t*) heap_caps_malloc(buf_pixels*sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_buf2 = (lv_color_t*) heap_caps_malloc(buf_pixels*sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (!lv_buf1 || !lv_buf2) { Serial.println("[fatal] LVGL buffers alloc failed"); for(;;) delay(1000); }

  disp = lv_display_create(800, 480);
  lv_display_set_flush_cb(disp, [](lv_display_t* display, const lv_area_t* area, uint8_t* px_map) {
    const int x = area->x1, y = area->y1;
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(x, y, (uint16_t*)px_map, w, h);
    lv_disp_flush_ready(display);
  });
  lv_display_set_buffers(disp, lv_buf1, lv_buf2, buf_pixels*sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);

  const esp_timer_create_args_t tick_args = { .callback=+[](void*){ lv_tick_inc(5); }, .arg=nullptr, .dispatch_method=ESP_TIMER_TASK, .name="lv_tick" };
  esp_timer_handle_t tick_timer; esp_timer_create(&tick_args, &tick_timer); esp_timer_start_periodic(tick_timer, 5000);

  // Build UI first so diagnostics are visible
  build_ui();

  // Immediate first refresh
  lv_timer_handler();
  lv_refr_now(NULL);

  // Scan & show on-screen
  i2c_scan_multiline("post-BL");

  // --- Touch auto-detect (from touch_input.cpp) ---
  touch_init_and_register_lvgl();
  if (touch_present()) {
    char buf[64];
    snprintf(buf, sizeof(buf), "touch: %s @0x%02X", touch_ic_name(), touch_i2c_address());
    Serial.println(buf);
    diag_set(buf);
  } else {
    Serial.println("touch: NOT detected");
    diag_set("touch: NOT detected");
  }

  // Final sanity: one more scan after touch init
  i2c_scan_multiline("post-touch");

  // Session updates
  lv_timer_create(session_timer_cb, 250, nullptr);

  // GT status HUD (every 250 ms)
  lv_timer_create(gt_status_timer_cb, 250, nullptr);

  print_mem("POST-begin");
}

/* -------------------------------- loop --------------------------------- */
void loop() {
  // UI interaction; touch_input.cpp provides LVGL pointer reads
  // Serial controls work (1..4 presets, q/a base, w/s beat, +/- minutes)
  lv_timer_handler();
  delay(2);
}
