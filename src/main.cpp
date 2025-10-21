/**
 * LVGL UI — Binaural Session (Readable Fonts Edition)
 * - Big fonts, high contrast, increased spacing
 * - Same functionality as previous LVGL build
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

/* ------------------------------ App Model ------------------------------ */

struct Preset {
  const char* name;
  float baseHz;
  float beatHz;
  float beatMin;
  float beatMax;
};

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
static float fLeft     = 0.0f;
static float fRight    = 0.0f;

static SessionState state = SessionState::IDLE;
static uint8_t  sessionMinutes = 10;
static uint32_t sessionStartMs = 0;
static uint32_t accumulatedMs  = 0;

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

/* ------------------------- Hardware: Backlight ------------------------- */
static constexpr int I2C_SDA  = 8;
static constexpr int I2C_SCL  = 9;
static constexpr int I2C_PORT = 0;
static CH422G *blExpander = nullptr;
static constexpr int EXIO_BL = 2; // EXIO2

static void backlight_on() {
  if (!blExpander) {
    Wire.begin(I2C_SDA, I2C_SCL);
    blExpander = new CH422G(I2C_PORT, I2C_SDA, I2C_SCL);
    if (!blExpander->begin()) { Serial.println("[BL] CH422G begin() FAILED"); return; }
  }
  blExpander->pinMode(EXIO_BL, OUTPUT);
  blExpander->digitalWrite(EXIO_BL, HIGH);
  Serial.println("[BL] Backlight enabled (EXIO2 HIGH)");
}

/* ------------------------ Hardware: RGB Display ------------------------ */
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

static constexpr int LV_BUF_LINES = 40;
static lv_color_t* lv_buf1 = nullptr;
static lv_color_t* lv_buf2 = nullptr;

static void lvgl_flush_cb(lv_display_t* display, const lv_area_t* area, uint8_t* px_map) {
  const int x = area->x1, y = area->y1;
  const int w = area->x2 - area->x1 + 1;
  const int h = area->y2 - area->y1 + 1;
  uint16_t* p = reinterpret_cast<uint16_t*>(px_map);
  gfx->draw16bitRGBBitmap(x, y, p, w, h);
  lv_disp_flush_ready(display);
}
static void lvgl_tick_cb(void*) { lv_tick_inc(5); }

/* ----------------------------- Utilities ------------------------------ */
static void print_mem(const char* tag) {
  Serial.printf("[%s] Heap free: %u KB | PSRAM size: %u KB | PSRAM free: %u KB\n",
    tag, (uint32_t)(ESP.getFreeHeap()/1024), (uint32_t)(ESP.getPsramSize()/1024), (uint32_t)(ESP.getFreePsram()/1024));
}
static void print_banner() {
  Serial.println("\n--- LVGL Session UI (Readable Fonts) ---");
  print_mem("BOOT/INFO");
  Serial.println("[OK] Controls: 1..4 presets | q/a base +/-5 | w/s beat +/-0.5 | r reset | g start | p pause/resume | x stop | +/- minutes | z refresh | h help | m mem");
}

/* --------------------------- Session helpers -------------------------- */
static void startSession() { if (state!=SessionState::RUNNING){ if (state==SessionState::DONE) accumulatedMs=0; state=SessionState::RUNNING; sessionStartMs=millis(); } }
static void pauseSessionToggle() {
  if (state==SessionState::RUNNING){ accumulatedMs += (millis()-sessionStartMs); state=SessionState::PAUSED; }
  else if (state==SessionState::PAUSED){ sessionStartMs=millis(); state=SessionState::RUNNING; }
}
static void stopSession() { state=SessionState::IDLE; accumulatedMs=0; }

/* ---------------------------- LVGL Styles ----------------------------- */
static lv_style_t style_bg;
static lv_style_t style_text_small;   // 20 pt
static lv_style_t style_text_large;   // 24 pt
static lv_style_t style_btn;

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
  char bv[32];
  snprintf(bv, sizeof(bv), "%.2f Hz", beatHz);
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
static void ui_sync_all() {
  computeEngine();
  ui_update_header();
  ui_update_lr();
  ui_update_slider();
  ui_update_minutes();
  ui_update_progress();
}

/* ------------------------------ Events ------------------------------- */
static void beat_slider_event_cb(lv_event_t* e) {
  lv_obj_t* slider = (lv_obj_t*) lv_event_get_target(e);
  int32_t v = lv_slider_get_value(slider);  // centi-Hz
  beatHz = v / 100.0f;
  ui_sync_all();
}
static void preset_btn_event_cb(lv_event_t* e) {
  uintptr_t idx = (uintptr_t) lv_event_get_user_data(e);
  presetIdx = (int)idx;
  baseHz = PRESETS[presetIdx].baseHz;
  beatHz = PRESETS[presetIdx].beatHz;
  ui_sync_all();
}
static void start_btn_event_cb(lv_event_t*) { startSession(); ui_update_progress(); }
static void pause_btn_event_cb(lv_event_t*) { pauseSessionToggle(); ui_update_progress(); }
static void stop_btn_event_cb(lv_event_t*)  { stopSession(); ui_update_progress(); }
static void minutes_minus_event_cb(lv_event_t*) { if (sessionMinutes>1)  sessionMinutes--; ui_update_minutes(); ui_update_progress(); }
static void minutes_plus_event_cb(lv_event_t*)  { if (sessionMinutes<60) sessionMinutes++; ui_update_minutes(); ui_update_progress(); }

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

  // Header (big)
  headerLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(headerLabel, &style_text_large, 0);
  lv_obj_set_pos(headerLabel, 12, 10);

  // L/R readout (big)
  lrLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(lrLabel, &style_text_large, 0);
  lv_obj_set_pos(lrLabel, 12, 48);

  // Preset buttons row (tall & wide)
  const int presY = 92, presX0 = 12, presW = 150, presH = 50, presGap = 10;
  for (int i = 0; i < 4; ++i) {
    presetBtns[i] = make_btn(lv_screen_active(), PRESETS[i].name, preset_btn_event_cb, (void*)(uintptr_t)i, presW, presH);
    lv_obj_set_pos(presetBtns[i], presX0 + i*(presW + presGap), presY);
  }

  // Beat slider + labels (big)
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

  // Duration spinbox + +/- (bigger)
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

  // Session control buttons (big)
  startBtn = make_btn(lv_screen_active(), "Start", start_btn_event_cb, nullptr, 150, 50);
  pauseBtn = make_btn(lv_screen_active(), "Pause/Resume", pause_btn_event_cb, nullptr, 190, 50);
  stopBtn  = make_btn(lv_screen_active(), "Stop",  stop_btn_event_cb,  nullptr, 150, 50);

  lv_obj_set_pos(startBtn,  250, 264);
  lv_obj_set_pos(pauseBtn,  410, 264);
  lv_obj_set_pos(stopBtn,   610, 264);

  // Time / Progress (big)
  timeLeftLabel = lv_label_create(lv_screen_active());
  lv_obj_add_style(timeLeftLabel, &style_text_large, 0);
  lv_obj_set_pos(timeLeftLabel, 12, 326);

  progressBar = lv_bar_create(lv_screen_active());
  lv_obj_set_size(progressBar, 776, 24);
  lv_obj_set_pos(progressBar, 12, 360);

  // Initial fill
  ui_sync_all();
}

/* ------------------------------- Serial ------------------------------- */
static void applyPreset(int idx) {
  presetIdx = idx; baseHz = PRESETS[idx].baseHz; beatHz = PRESETS[idx].beatHz; ui_sync_all();
}
static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case '1': applyPreset(0); break;
      case '2': applyPreset(1); break;
      case '3': applyPreset(2); break;
      case '4': applyPreset(3); break;
      case 'q': baseHz += 5.0f; break;
      case 'a': baseHz -= 5.0f; if (baseHz<20.0f) baseHz=20.0f; break;
      case 'w': beatHz += 0.5f; break;
      case 's': beatHz -= 0.5f; break;
      case 'r': baseHz = PRESETS[presetIdx].baseHz; beatHz = PRESETS[presetIdx].beatHz; break;
      case 'g': startSession(); break;
      case 'p': pauseSessionToggle(); break;
      case 'x': stopSession(); break;
      case '+': if (sessionMinutes<60) sessionMinutes++; break;
      case '-': if (sessionMinutes>1)  sessionMinutes--; break;
      case 'h': print_banner(); break;
      case 'm': print_mem("NOW"); break;
      case 'z': default: break;
    }
    ui_sync_all();
  }
}

/* ------------------------ Timers / Lifecycle -------------------------- */
static void session_timer_cb(lv_timer_t*) { ui_update_progress(); }

void setup() {
  Serial.begin(115200);
  delay(150);
  print_banner();

  if (ESP.getPsramSize() < 4*1024*1024) { Serial.println("[ERR] PSRAM missing."); for(;;) delay(1000); }

  backlight_on();
  if (!gfx->begin()) { Serial.println("[ERR] gfx->begin() failed"); for(;;) delay(1000); }

  lv_init();

  size_t buf_pixels = 800 * LV_BUF_LINES;
  lv_buf1 = (lv_color_t*) heap_caps_malloc(buf_pixels*sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_buf2 = (lv_color_t*) heap_caps_malloc(buf_pixels*sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (!lv_buf1 || !lv_buf2) { Serial.println("[ERR] LVGL buffer alloc failed"); for(;;) delay(1000); }

  lv_display_t* display = lv_display_create(800, 480);
  lv_display_set_flush_cb(display, lvgl_flush_cb);
  lv_display_set_buffers(display, lv_buf1, lv_buf2, buf_pixels*sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
  disp = display;

  const esp_timer_create_args_t tick_args = { .callback=&lvgl_tick_cb, .arg=nullptr, .dispatch_method=ESP_TIMER_TASK, .name="lv_tick" };
  esp_timer_handle_t tick_timer; esp_timer_create(&tick_args, &tick_timer); esp_timer_start_periodic(tick_timer, 5000);

  computeEngine();
  build_ui();

  lv_timer_create(session_timer_cb, 250, nullptr);

  print_mem("POST-begin");
  Serial.println("[OK] UI up with large fonts.");
}

void loop() {
  handleSerial();
  lv_timer_handler();
  delay(2);
}
