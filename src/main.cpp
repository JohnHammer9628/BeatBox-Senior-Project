// src/main.cpp
/**
 * Baby Monitor UI (ESP32-S3 + 4.3" RGB + GT911 + CH422G)
 * Centering fix: use proper RGB porch timings (no software shift).
 * Changes:
 *   - Horizontal porches set to HFP=108, HSYNC=48, HBP=20 (was 40/48/88)
 *   - Removed all software X-offset code
 *   - Touch pipeline unchanged
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
#include "touch_input.h"

/* ------------------------- Pins / I2C ------------------------- */
static constexpr int I2C_SDA  = 8;
static constexpr int I2C_SCL  = 9;

/* ------------------------- CH422G ----------------------------- */
static constexpr int I2C_PORT = 0;
static CH422G *exio = nullptr;
static bool exio_ok = false;
static constexpr int EXIO_BL = 2;

/* -------------------- (Optional) drive strength --------------- */
#include "driver/gpio.h"
static inline void boost_rgb_drive() {
  gpio_set_drive_capability((gpio_num_t)5,  GPIO_DRIVE_CAP_3); // DE
  gpio_set_drive_capability((gpio_num_t)3,  GPIO_DRIVE_CAP_3); // VS
  gpio_set_drive_capability((gpio_num_t)46, GPIO_DRIVE_CAP_3); // HS
  gpio_set_drive_capability((gpio_num_t)7,  GPIO_DRIVE_CAP_3); // PCLK
}

/* ------------------------ Display HW -------------------------- */
/* Porch timing tuned to shift active area LEFT (reduce left black band) */
static constexpr int HFP     = 108;  // front porch (pixels)
static constexpr int HSYNC   = 48;   // HSYNC pulse width
static constexpr int HBP     = 20;   // back porch  (pixels)
static constexpr int VFP     = 13;
static constexpr int VSYNC   = 3;
static constexpr int VBP     = 32;
static constexpr int PCLK_HZ = 16000000;

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  /* DE,VS,HS,PCLK */ 5, 3, 46, 7,
  /* R0..R4 */ 1, 2, 42, 41, 40,
  /* G0..G5 */ 39, 0, 45, 48, 47, 21,
  /* B0..B4 */ 14, 38, 18, 17, 10,
  /* hsync_pol */ 0, /* hfp,hsync,hbp */ HFP, HSYNC, HBP,
  /* vsync_pol */ 0, /* vfp,vsync,vbp */ VFP, VSYNC, VBP,
  /* pclk_neg */ 1,  /* prefer speed */ PCLK_HZ
);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(800, 480, rgbpanel, 0, false);

/* ------------------------------ App Model ------------------------------ */
enum class PlayPreset : uint8_t { None=0, WhiteNoise, Rain, Heartbeat, Lullaby };
static struct {
  uint8_t  soundLevel;         // 0..100 (simulated)
  bool     cryLikely;
  bool     motion;
  uint32_t lastMotionMs;
  PlayPreset playing;
  uint8_t  volume;             // 0..100
  uint8_t  cryThresh;          // threshold (sim)
} g = {0,false,false,0, PlayPreset::None, 60, 65};

/* ----------------------------- LVGL glue ------------------------------ */
static lv_display_t* disp;
static lv_obj_t*  headerLabel;
static lv_obj_t*  soundLabel;
static lv_obj_t*  soundBar;
static lv_obj_t*  cryBadge;
static lv_obj_t*  motionLabel;
static lv_obj_t*  nowPlayingLabel;
static lv_obj_t*  playBtn;
static lv_obj_t*  stopBtn;
static lv_obj_t*  volumeSlider;
static lv_obj_t*  volumeValueLabel;

static lv_obj_t*  diagLabel = nullptr;
static lv_obj_t*  scanBox = nullptr;

/* LVGL buffers */
static constexpr int LV_BUF_LINES = 40;
static lv_color_t* lv_buf1 = nullptr;
static lv_color_t* lv_buf2 = nullptr;

/* ---------------------------- Styles ----------------------------- */
static lv_style_t style_bg, style_text_small, style_text_large, style_btn, style_scan, style_badge_ok, style_badge_warn;

static void init_styles() {
  lv_style_init(&style_bg);
  lv_style_set_bg_color(&style_bg, lv_color_hex(0x000000));
  lv_style_set_text_color(&style_bg, lv_color_hex(0xFFFFFF));
  lv_style_set_outline_width(&style_bg, 0);
  lv_style_set_border_width(&style_bg, 0);
  lv_obj_add_style(lv_screen_active(), &style_bg, 0);
  lv_obj_clear_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);

  lv_style_init(&style_text_small);
  lv_style_set_text_color(&style_text_small, lv_color_hex(0xFFFFFF));
  lv_style_set_text_font(&style_text_small, &lv_font_montserrat_20);

  lv_style_init(&style_text_large);
  lv_style_set_text_color(&style_text_large, lv_color_hex(0xFFFFFF));
  lv_style_set_text_font(&style_text_large, &lv_font_montserrat_24);

  lv_style_init(&style_btn);
  lv_style_set_bg_color(&style_btn, lv_color_hex(0x303030));
  lv_style_set_radius(&style_btn, 10);
  lv_style_set_pad_all(&style_btn, 10);
  lv_style_set_outline_width(&style_btn, 0);
  lv_style_set_border_width(&style_btn, 0);

  lv_style_init(&style_scan);
  lv_style_set_text_color(&style_scan, lv_color_hex(0xFFFF00));
  lv_style_set_text_font(&style_scan, &lv_font_montserrat_20);

  lv_style_init(&style_badge_ok);
  lv_style_set_bg_color(&style_badge_ok, lv_color_hex(0x2e7d32));
  lv_style_set_radius(&style_badge_ok, 8);
  lv_style_set_pad_hor(&style_badge_ok, 10);
  lv_style_set_pad_ver(&style_badge_ok, 6);

  lv_style_init(&style_badge_warn);
  lv_style_set_bg_color(&style_badge_warn, lv_color_hex(0xc62828));
  lv_style_set_radius(&style_badge_warn, 8);
  lv_style_set_pad_hor(&style_badge_warn, 10);
  lv_style_set_pad_ver(&style_badge_warn, 6);
}

/* ---------------------- Helpers ---------------------- */
static inline uint32_t since(uint32_t t_ms) { uint32_t now=millis(); return (now>=t_ms)? (now - t_ms) : (0xFFFFFFFFu - t_ms + now + 1); }
static inline const char* presetName(PlayPreset p) {
  switch (p) { case PlayPreset::WhiteNoise: return "White Noise";
               case PlayPreset::Rain:       return "Rain";
               case PlayPreset::Heartbeat:  return "Heartbeat";
               case PlayPreset::Lullaby:    return "Lullaby";
               default:                     return "None"; }
}
static inline uint8_t clamp100(int v){ if(v<0) return 0; if(v>100) return 100; return (uint8_t)v; }

/* ---------------------- UI update fns ---------------------- */
static lv_obj_t* make_btn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb, int w=120, int h=48) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_add_style(btn, &style_btn, 0);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_ANY);
  lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_ANY);
  if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* lb = lv_label_create(btn);
  lv_obj_add_style(lb, &style_text_small, 0);
  lv_label_set_text(lb, txt);
  lv_obj_center(lb);
  return btn;
}

static void update_header() { lv_label_set_text(headerLabel, "Baby Monitor — Local Status"); }

static void update_sound() {
  lv_bar_set_range(soundBar, 0, 100);
  lv_bar_set_value(soundBar, g.soundLevel, LV_ANIM_OFF);

  char buf[64];
  snprintf(buf, sizeof(buf), "Sound Level: %u/100", (unsigned)g.soundLevel);
  lv_label_set_text(soundLabel, buf);

  lv_obj_clean(cryBadge);
  lv_obj_add_style(cryBadge, (g.cryLikely ? &style_badge_warn : &style_badge_ok), 0);
  lv_obj_t* lbl = lv_label_create(cryBadge);
  lv_obj_add_style(lbl, &style_text_small, 0);
  lv_label_set_text(lbl, g.cryLikely ? "CRY LIKELY" : "Calm");
  lv_obj_center(lbl);
}

static void update_motion() {
  char buf[128];
  uint32_t ago = since(g.lastMotionMs)/1000u;
  snprintf(buf, sizeof(buf), "Motion: %s  •  Last movement: %lus ago",
           g.motion ? "detected" : "idle", (unsigned)ago);
  lv_label_set_text(motionLabel, buf);
}

static void update_now_playing() {
  char np[96];
  snprintf(np, sizeof(np), "Now Playing: %s", presetName(g.playing));
  lv_label_set_text(nowPlayingLabel, np);

  char vv[24]; snprintf(vv, sizeof(vv), "%u%%", (unsigned)g.volume);
  lv_label_set_text(volumeValueLabel, vv);

  lv_slider_set_range(volumeSlider, 0, 100);
  lv_slider_set_value(volumeSlider, g.volume, LV_ANIM_OFF);
}

static void ui_refresh_all() {
  update_header();
  update_sound();
  update_motion();
  update_now_playing();
}

/* ------------------------- Events ------------------------- */
static void on_play(lv_event_t*) { if (g.playing == PlayPreset::None) g.playing = PlayPreset::WhiteNoise; ui_refresh_all(); }
static void on_stop(lv_event_t*) { g.playing = PlayPreset::None; ui_refresh_all(); }
static void on_volume(lv_event_t* e) {
  g.volume = clamp100(lv_slider_get_value((lv_obj_t*)lv_event_get_target(e)));
  char vv[24]; snprintf(vv, sizeof(vv), "%u%%", (unsigned)g.volume);
  lv_label_set_text(volumeValueLabel, vv);
}

/* ------------------------- Build UI ------------------------- */
static void build_ui() {
  init_styles();

  headerLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(headerLabel, &style_text_large, 0);
  lv_obj_set_pos(headerLabel, 12, 10);

  // I2C scan box (left column)
  scanBox = lv_label_create(lv_screen_active());
  lv_obj_add_style(scanBox, &style_scan, 0);
  lv_obj_set_width(scanBox, 360);
  lv_label_set_long_mode(scanBox, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(scanBox, 12, 44);

  // Cry badge (top-right area)
  cryBadge = lv_obj_create(lv_screen_active());
  lv_obj_set_size(cryBadge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_pos(cryBadge, 600, 12);

  // Sound label + bar
  soundLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(soundLabel, &style_text_large, 0);
  lv_obj_set_pos(soundLabel, 12, 150);

  soundBar = lv_bar_create(lv_screen_active());
  lv_obj_set_size(soundBar, 760, 22);
  lv_obj_set_pos(soundBar, 12, 186);

  // Motion line
  motionLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(motionLabel, &style_text_large, 0);
  lv_obj_set_pos(motionLabel, 12, 222);

  // Now Playing row
  nowPlayingLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(nowPlayingLabel, &style_text_large, 0);
  lv_obj_set_pos(nowPlayingLabel, 12, 270);

  // Controls row (play/stop/volume)
  playBtn = make_btn(lv_screen_active(), "Play", on_play, 120, 48);
  lv_obj_set_pos(playBtn, 12, 308);

  stopBtn = make_btn(lv_screen_active(), "Stop", on_stop, 120, 48);
  lv_obj_set_pos(stopBtn, 152, 308);

  volumeSlider = lv_slider_create(lv_screen_active());
  lv_obj_set_size(volumeSlider, 420, 22);
  lv_obj_set_pos(volumeSlider, 292, 316);
  lv_obj_add_event_cb(volumeSlider, on_volume, LV_EVENT_VALUE_CHANGED, nullptr);

  volumeValueLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(volumeValueLabel, &style_text_large, 0);
  lv_obj_set_style_text_color(volumeValueLabel, lv_color_hex(0xFFFF00), 0);
  lv_obj_set_pos(volumeValueLabel, 724, 312);

  // Diag label (bottom left)
  diagLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(diagLabel, &style_text_small, 0);
  lv_obj_set_pos(diagLabel, 12, 430);
  lv_label_set_text(diagLabel, "diag: ready");

  ui_refresh_all();
}

/* ------------------------------- Serial ------------------------------- */
static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case 'p': on_play(nullptr); break;
      case 'x': on_stop(nullptr); break;
      case '+': g.volume = clamp100(g.volume+5); update_now_playing(); break;
      case '-': g.volume = clamp100(g.volume-5); update_now_playing(); break;
      case 'w': g.cryThresh = clamp100(g.cryThresh+2); break;
      case 's': g.cryThresh = clamp100(g.cryThresh-2); break;
      default: break;
    }
  }
}

/* -------- I2C scan helper: multi-line -------- */
static void scan_set(const char* s) { if (scanBox) lv_label_set_text(scanBox, s); }
static void i2c_scan_multiline(const char* tag) {
  String out; out.reserve(256);
  out += "I2C "; out += (tag ? tag : ""); out += ":\n";
  int col = 0;
  for (uint8_t a = 1; a < 127; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      char buf[8];
      snprintf(buf, sizeof(buf), "0x%02X ", a);
      out += buf; col++; if (col >= 12) { out += "\n"; col = 0; }
    }
  }
  if (col == 0 && out.indexOf('\n') == (int)out.length()-1) { out += "(none)"; }
  Serial.println(out);
  scan_set(out.c_str());
}

/* ---------------------- GT911 reset via CH422G ------------------------ */
static bool gt_reset_seq(int exio_int, int exio_rst) {
  if (!exio_ok) return false;
  exio->pinMode(exio_int, OUTPUT);
  exio->pinMode(exio_rst, OUTPUT);
  exio->digitalWrite(exio_int, HIGH);
  exio->digitalWrite(exio_rst, HIGH);
  delay(2);

  exio->digitalWrite(exio_int, LOW);
  delay(1);

  exio->digitalWrite(exio_rst, LOW);  delay(10);
  exio->digitalWrite(exio_rst, HIGH); delay(10);

  exio->pinMode(exio_int, INPUT);
  exio->digitalWrite(exio_rst, HIGH);

  delay(20);

  Wire.beginTransmission(0x5D);
  bool ok = (Wire.endTransmission() == 0);
  Serial.printf("[*] GT911 reset via CH422G: INT=EXIO%d RST=EXIO%d -> %s\n",
                exio_int, exio_rst, ok ? "0x5D ACK" : "NO ACK");
  return ok;
}
static bool try_gt_reset() {
  if (!exio_ok) return false;
  if (gt_reset_seq(7, 6)) { Serial.println("[*] Mapping A OK (INT=EXIO7, RST=EXIO6)"); return true; }
  if (gt_reset_seq(6, 7)) { Serial.println("[*] Mapping B OK (INT=EXIO6, RST=EXIO7)"); return true; }
  Serial.println("[*] No ACK after A/B reset (will continue anyway)");
  return false;
}

/* ------------------------ Simulated data timer -------------------------- */
static uint32_t cry_high_since = 0;
static void tick_sim_timer(lv_timer_t*) {
  static uint32_t t0 = millis();
  float t = (millis() - t0) / 1000.0f;
  int base = (int)(50 + 35 * sinf(t * 1.7f));
  int noise = (int)(rand() % 11) - 5;
  g.soundLevel = clamp100(base + noise);

  if (g.soundLevel > g.cryThresh) {
    if (!g.cryLikely) {
      if (cry_high_since == 0) cry_high_since = millis();
      if (millis() - cry_high_since > 600) g.cryLikely = true;
    }
  } else {
    g.cryLikely = false;
    cry_high_since = 0;
  }

  if (since(g.lastMotionMs) > 10000) g.motion = false;

  update_sound();
  update_motion();
}

/* ------------------------ Timers / Lifecycle -------------------------- */
static void session_timer_cb(lv_timer_t*) { }

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
  (void)i2c_bus_recover(); // safe

  // --- CH422G (guard) ---
  exio = new CH422G(I2C_PORT, I2C_SDA, I2C_SCL);
  if (exio && exio->begin()) {
    exio_ok = true;
    exio->pinMode(EXIO_BL, OUTPUT);
    exio->digitalWrite(EXIO_BL, LOW);     // BL OFF until first clean frame
    Serial.println("[exio] EXIO2 -> LOW (BL off)");
    for (int pin = 0; pin < 8; ++pin) { if (pin != EXIO_BL) exio->pinMode(pin, INPUT); }
    Serial.println("[exio] EXIO[others] -> INPUT (released)");
    try_gt_reset();
  } else {
    exio_ok = false;
    Serial.println("[exio] CH422G begin() failed (continuing)");
  }
  delay(40);

  Wire.setClock(400000); // faster after bring-up

  // --- Display + LVGL ---
  boost_rgb_drive();
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
  lv_display_set_default(disp);

  const esp_timer_create_args_t tick_args = { .callback=+[](void*){ lv_tick_inc(5); }, .arg=nullptr, .dispatch_method=ESP_TIMER_TASK, .name="lv_tick" };
  esp_timer_handle_t tick_timer; esp_timer_create(&tick_args, &tick_timer); esp_timer_start_periodic(tick_timer, 5000);

  // Build UI
  build_ui();

  // Render a clean frame, then enable BL (only if exio_ok)
  gfx->fillScreen(BLACK);
  lv_timer_handler();
  lv_refr_now(NULL);
  delay(150);
  if (exio_ok) {
    exio->digitalWrite(EXIO_BL, HIGH);
    Serial.println("[exio] EXIO2 -> HIGH (BL on)");
  }

  // Scan & show on-screen
  i2c_scan_multiline("post-BL");

  // --- Touch auto-detect ---
  touch_init_and_register_lvgl();
  if (touch_present()) {
    char buf[64];
    snprintf(buf, sizeof(buf), "touch: %s @0x%02X", touch_ic_name(), touch_i2c_address());
    Serial.println(buf);
    if (diagLabel) lv_label_set_text(diagLabel, buf);
  } else {
    Serial.println("touch: NOT detected");
    if (diagLabel) lv_label_set_text(diagLabel, "touch: NOT detected");
  }

  // Final sanity: one more scan after touch init
  i2c_scan_multiline("post-touch");

  // Timers
  lv_timer_create(session_timer_cb, 500, nullptr);
  lv_timer_create(tick_sim_timer,    120, nullptr);

  g.lastMotionMs = millis();
}

/* -------------------------------- loop --------------------------------- */
void loop() {
  lv_timer_handler();
  handleSerial();
  delay(2);
}
