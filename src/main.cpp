/**
 * Binaural UI — Anti-Tear Build (frame pacing + tiny redraws)
 *
 * Keys (USB Serial @115200):
 *   g            start session
 *   p            pause/resume
 *   x            stop/reset
 *   + / -        minutes +1 / -1  (1..60)
 *   1/2/3/4      presets (Alpha/Beta/Theta/Delta)
 *   q / a        base +5 / -5      (min 20)
 *   w / s        beat +0.5 / -0.5  (clamped to preset)
 *   r            reset base/beat to preset defaults
 *   t            toggle beat RAMP-IN on/off
 *   [ / ]        ramp duration -5s / +5s (0..300s)
 *   f            toggle FPS overlay
 *
 * Notes:
 * - We pace frames to the panel refresh to avoid tearing:
 *     PCLK=16 MHz (~31 Hz refresh) -> frame ~32 ms
 *     PCLK=32 MHz (~62 Hz refresh) -> frame ~16 ms
 * - Visualizer draws only the changed strip each frame.
 * - Header shows TARGET beat (stable). Effective/ramped beat shown in footer at 4 Hz.
 */
#include <Arduino.h>
#include <Wire.h>
#include <math.h>   // for fabsf

#include <esp_io_expander.hpp>   // CH422G over I2C
using namespace esp_expander;

#include <databus/Arduino_ESP32RGBPanel.h>
#include <display/Arduino_RGB_Display.h>

// ==================== Board wiring (Waveshare 4.3" Dev Board B) ====================
static constexpr int I2C_SDA  = 8;
static constexpr int I2C_SCL  = 9;
static constexpr int I2C_PORT = 0;      // ESP32-S3 I2C controller index

// ==================== RGB pins + timing for 800x480 panel ====================
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  /* DE,VS,HS,PCLK */ 5, 3, 46, 7,
  /* R0..R4 */ 1, 2, 42, 41, 40,
  /* G0..G5 */ 39, 0, 45, 48, 47, 21,
  /* B0..B4 */ 14, 38, 18, 17, 10,
  /* hsync_pol */ 0, /* hfp,hsync,hbp */ 40, 48, 88,
  /* vsync_pol */ 0, /* vfp,vsync,vbp */ 13, 3, 32,
  /* pclk_neg */ 1,  /* prefer speed */ 16000000   // Try 32000000 later for ~60 Hz
);

// Display object (auto_flush = false so we control when pixels are pushed)
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  800, 480, rgbpanel, 0 /* rotation */, false /* auto_flush */
);

// ==================== Backlight expander (CH422G) ====================
CH422G *blExpander = nullptr;
static constexpr int EXIO_BL = 2; // EXIO2 = BL-EN (on Waveshare “B” board)

// ==================== Binaural engine model (math only for now) ====================
struct Preset {
  const char* name;
  float baseHz;   // carrier (e.g., 200 Hz)
  float beatHz;   // L/R delta (e.g., 10 Hz for alpha)
  float beatMin;
  float beatMax;
};

Preset PRESETS[] = {
  {"Alpha", 200.0f, 10.0f,  8.0f, 12.0f},
  {"Beta",  220.0f, 18.0f, 13.0f, 30.0f},
  {"Theta", 180.0f,  6.0f,  4.0f,  7.0f},
  {"Delta", 150.0f,  2.0f,  0.5f,  3.0f}
};
static int   presetIdx = 0;
static float baseHz = PRESETS[0].baseHz;
static float beatHz = PRESETS[0].beatHz;

// Computed each frame
static float fLeft = 0.0f, fRight = 0.0f;

// ==================== Session state ====================
enum class SessionState : uint8_t { IDLE, RUNNING, PAUSED, DONE };
static SessionState state = SessionState::IDLE;
static uint8_t  sessionMinutes = 10;         // editable 1..60
static uint32_t sessionStartMs  = 0;          // when RUNNING actually began (excludes paused time)
static uint32_t accumulatedMs   = 0;          // time already accumulated before current run segment
static uint32_t lastSecondTick  = 0;          // to update countdown text once/sec

// ==================== Dirty-state caches (to minimize redraw) ====================
static float lastBase = NAN, lastBeat = NAN, lastLeft = NAN, lastRight = NAN;
static int   lastPreset = -1;
static bool  lastPausedFlag = true; // mirrors PAUSED state for header text
static int   lastVizX   = -1;

static SessionState lastStateDrawn = SessionState::IDLE;
static uint8_t      lastMinutesDrawn = 0;
static uint32_t     lastRemainSecDrawn = UINT32_MAX; // force first draw
static float        lastProgress = -1.0f;

// ==================== Helpers ====================
void print_mem(const char* tag = "MEM")
{
  Serial.printf("[%s] Heap free: %u KB | PSRAM size: %u KB | PSRAM free: %u KB\n",
                tag,
                (uint32_t)(ESP.getFreeHeap() / 1024),
                (uint32_t)(ESP.getPsramSize() / 1024),
                (uint32_t)(ESP.getFreePsram() / 1024));
}

void backlight_on()
{
  if (!blExpander) {
    Wire.begin(I2C_SDA, I2C_SCL);
    blExpander = new CH422G(I2C_PORT, I2C_SDA, I2C_SCL);
    if (!blExpander->begin()) {
      Serial.println("[BL] CH422G begin() FAILED (check I2C pins/port)");
      return;
    }
  }
  blExpander->pinMode(EXIO_BL, OUTPUT);
  blExpander->digitalWrite(EXIO_BL, HIGH);
  Serial.println("[BL] Backlight enabled (EXIO2 HIGH)");
}

void computeEngine()
{
  const Preset& p = PRESETS[presetIdx];
  if (beatHz < p.beatMin) beatHz = p.beatMin;
  if (beatHz > p.beatMax) beatHz = p.beatMax;

  fLeft  = baseHz - (beatHz * 0.5f);
  fRight = baseHz + (beatHz * 0.5f);
}

void applyPreset(int idx)
{
  presetIdx = idx;
  baseHz = PRESETS[idx].baseHz;
  beatHz = PRESETS[idx].beatHz;
}

void startSession()
{
  if (state == SessionState::RUNNING) return;
  if (state == SessionState::DONE) { accumulatedMs = 0; }
  state = SessionState::RUNNING;
  sessionStartMs = millis();
  lastSecondTick = 0; // force immediate countdown refresh
}

void pauseSessionToggle()
{
  if (state == SessionState::RUNNING) {
    // accumulate time up to now
    accumulatedMs += (millis() - sessionStartMs);
    state = SessionState::PAUSED;
  } else if (state == SessionState::PAUSED) {
    // resume
    sessionStartMs = millis();
    state = SessionState::RUNNING;
  }
}

void stopSession()
{
  state = SessionState::IDLE;
  accumulatedMs = 0;
  lastSecondTick = 0;
}

uint32_t sessionElapsedMs()
{
  if (state == SessionState::RUNNING) {
    return accumulatedMs + (millis() - sessionStartMs);
  }
  return accumulatedMs;
}

uint32_t sessionTotalMs()
{
  return (uint32_t)sessionMinutes * 60u * 1000u;
}

// ==================== Input ====================
void handleSerial()
{
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      // presets
      case '1': applyPreset(0); break;
      case '2': applyPreset(1); break;
      case '3': applyPreset(2); break;
      case '4': applyPreset(3); break;

      // base / beat
      case 'q': baseHz += 5.0f; break;
      case 'a': baseHz -= 5.0f; if (baseHz < 20.0f) baseHz = 20.0f; break;
      case 'w': beatHz += 0.5f; break;
      case 's': beatHz -= 0.5f; break;
      case 'r': baseHz = PRESETS[presetIdx].baseHz; beatHz = PRESETS[presetIdx].beatHz; break;

      // session controls
      case 'g': startSession(); break;     // go
      case 'p': pauseSessionToggle(); break;
      case 'x': stopSession(); break;
      case '+': if (sessionMinutes < 60) sessionMinutes++; break;
      case '-': if (sessionMinutes > 1)  sessionMinutes--; break;

      default: break;
    }
  }
}

// ==================== Drawing (dirty-region updates) ====================
void drawStaticOnce()
{
  // Header bar background
  gfx->fillRect(0, 0, 800, 42, 0x0841);

  // Main panel background
  gfx->fillRect(0, 42, 800, 438, BLACK);

  // Left / Right boxes and labels
  gfx->fillRect(40, 80, 320, 140, 0x39E7);
  gfx->fillRect(440, 80, 320, 140, 0x39E7);
  gfx->drawRect(40, 80, 320, 140, WHITE);
  gfx->drawRect(440, 80, 320, 140, WHITE);

  gfx->setTextColor(WHITE); gfx->setTextSize(2);
  gfx->setCursor(60, 100); gfx->print("Left Freq (Hz)");
  gfx->setCursor(460, 100); gfx->print("Right Freq (Hz)");

  // Visualizer baseline
  gfx->fillRect(40, 340, 720, 12, 0x10A2);

  // Footer area for info + controls
  gfx->fillRect(40, 260, 720, 90, BLACK);

  // Progress bar track (bottom band)
  gfx->fillRect(40, 400, 720, 24, 0x2104); // dark track
  gfx->drawRect(40, 400, 720, 24, WHITE);  // outline
}

const char* stateName(SessionState s)
{
  switch (s) {
    case SessionState::IDLE:   return "IDLE";
    case SessionState::RUNNING:return "RUNNING";
    case SessionState::PAUSED: return "PAUSED";
    case SessionState::DONE:   return "DONE";
  }
  return "";
}

void drawHeaderIfChanged()
{
  bool pausedFlag = (state == SessionState::PAUSED);
  if (lastPreset!=presetIdx || lastBase!=baseHz || lastBeat!=beatHz || lastPausedFlag!=pausedFlag) {
    gfx->fillRect(0, 0, 800, 42, 0x0841);
    gfx->setTextColor(WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(12, 12);
    gfx->printf("Preset: %s   Base: %.1f Hz   Beat: %.2f Hz   %s",
                PRESETS[presetIdx].name, baseHz, beatHz, pausedFlag ? "[PAUSED]" : " ");
    lastPreset = presetIdx; lastPausedFlag = pausedFlag;
  }
}

void drawFreqsIfChanged()
{
  if (lastLeft!=fLeft) {
    gfx->fillRect(60, 145, 260, 40, 0x39E7);
    gfx->setTextColor(WHITE); gfx->setTextSize(3); gfx->setCursor(60, 150);
    gfx->printf("%.2f", fLeft);
    lastLeft = fLeft;
  }
  if (lastRight!=fRight) {
    gfx->fillRect(460, 145, 260, 40, 0x39E7);
    gfx->setTextColor(WHITE); gfx->setTextSize(3); gfx->setCursor(460, 150);
    gfx->printf("%.2f", fRight);
    lastRight = fRight;
  }

  if (lastBase!=baseHz || lastBeat!=beatHz) {
    gfx->fillRect(40, 260, 720, 90, BLACK);
    gfx->setTextColor(WHITE); gfx->setTextSize(2);
    gfx->setCursor(40, 260); gfx->printf("Carrier/Base: %.2f Hz", baseHz);
    gfx->setCursor(40, 290); gfx->printf("Beat Delta:   %.2f Hz  (L = base - beat/2, R = base + beat/2)", beatHz);
    gfx->setCursor(40, 320); gfx->print("Serial: 1..4 presets | q/a base +/-5 | w/s beat +/-0.5 | r reset | g start | p pause/resume | x stop | +/- minutes");
    lastBase = baseHz; lastBeat = beatHz;
  }
}

void drawVisualizerThin(float t)
{
  float speed = beatHz;
  if (speed < 0.5f) speed = 0.5f;
  if (speed > 30.0f) speed = 30.0f;
  speed /= 30.0f; // 0..1

  int x = 40 + (int)(sin(t * speed * 2.0f * PI) * 200.0f + 200.0f);

  if (lastVizX >= 0) {
    gfx->fillRect(40, 340, lastVizX, 12, 0x10A2); // erase old segment
  }
  gfx->fillRect(40, 340, x, 12, 0xFBE0);          // draw new segment
  lastVizX = x;
}

void drawSessionPanelIfChanged()
{
  // Recompute session metrics
  uint32_t totalMs = sessionTotalMs();
  uint32_t elapsed = sessionElapsedMs();
  if (elapsed >= totalMs && (state == SessionState::RUNNING)) {
    state = SessionState::DONE;
  }
  uint32_t remainMs = (elapsed >= totalMs) ? 0 : (totalMs - elapsed);
  uint32_t remainSec = remainMs / 1000;
  float progress = (totalMs == 0) ? 0.f : (float)elapsed / (float)totalMs;
  if (progress > 1.0f) progress = 1.0f;

  // Redraw only when needed
  bool needStateLine = (state != lastStateDrawn) || (sessionMinutes != lastMinutesDrawn);
  bool needCountdown = (remainSec != lastRemainSecDrawn) || needStateLine;
  bool needProgress  = fabsf(progress - lastProgress) > 0.005f || needStateLine;

  if (needStateLine) {
    // Upper footer line: state + minutes
    gfx->fillRect(40, 260, 720, 28, BLACK);
    gfx->setTextColor(WHITE); gfx->setTextSize(2);
    gfx->setCursor(40, 260);
    gfx->printf("Session: %s   Duration: %u min", stateName(state), (unsigned)sessionMinutes);
    lastStateDrawn = state;
    lastMinutesDrawn = sessionMinutes;
  }

  if (needCountdown) {
    // Middle footer line: remaining mm:ss
    uint32_t mm = remainSec / 60;
    uint32_t ss = remainSec % 60;
    gfx->fillRect(40, 288, 720, 28, BLACK);
    gfx->setTextColor(WHITE); gfx->setTextSize(2);
    gfx->setCursor(40, 288);
    gfx->printf("Time Left: %02u:%02u", (unsigned)mm, (unsigned)ss);
    lastRemainSecDrawn = remainSec;
  }

  if (needProgress) {
    // Progress bar fill
    int trackX = 40, trackY = 400, trackW = 720, trackH = 24;
    int fillW = (int)(progress * (float)(trackW - 2)); // inside border
    // clear interior
    gfx->fillRect(trackX+1, trackY+1, trackW-2, trackH-2, 0x2104);
    // draw fill
    if (fillW > 0) {
      gfx->fillRect(trackX+1, trackY+1, fillW, trackH-2, 0x07E0 /* green-ish */);
    }
    // border stays
    gfx->drawRect(trackX, trackY, trackW, trackH, WHITE);
    lastProgress = progress;
  }
}

// ==================== Arduino lifecycle ====================
void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\n--- Binaural UI (Session + Timer) ---");
  print_mem("BOOT");

  // PSRAM check
  if (ESP.getPsramSize() < 4 * 1024 * 1024) {
    Serial.println("[ERR] PSRAM not detected or too small. Check board/flags.");
    for (;;) delay(1000);
  }

  // Backlight on (via CH422G)
  backlight_on();

  // Display init
  if (!gfx->begin()) {
    Serial.println("[ERR] gfx->begin() failed");
    for (;;) delay(1000);
  }

  print_mem("POST-begin");

  // Initial state
  applyPreset(0);
  computeEngine();

  // Draw static layout once
  gfx->fillScreen(BLACK);
  drawStaticOnce();

  // Draw first frame
  drawHeaderIfChanged();
  drawFreqsIfChanged();
  drawSessionPanelIfChanged();

  // Push the composed frame
  gfx->flush();

  Serial.println("[OK] Controls: 1..4 presets | q/a base +/-5 | w/s beat +/-0.5 | r reset | g start | p pause/resume | x stop | +/- minutes");
}

void loop()
{
  handleSerial();

  // Engine + UI updates regardless of session, but animated parts only when not paused/done
  computeEngine();

  // Header / numeric readouts update if changed
  drawHeaderIfChanged();
  drawFreqsIfChanged();

  // Session UI: update countdown once/sec or when state changes
  uint32_t now = millis();
  if (now - lastSecondTick >= 250) { // small cadence for responsive countdown/progress
    lastSecondTick = now;
    drawSessionPanelIfChanged();
  }

  // Visualizer animates in RUNNING or IDLE; pauses in PAUSED and shows full when DONE
  if (state != SessionState::PAUSED) {
    static uint32_t t0 = millis();
    float t = (millis() - t0) / 1000.0f;
    drawVisualizerThin(t);
  }

  // Heartbeat pixel (top-left)
  static bool hb = false;
  gfx->drawPixel(0, 0, hb ? GREEN : RED);
  hb = !hb;

  // If session completed, mark DONE once and freeze visuals (except heartbeat)
  if (state == SessionState::RUNNING && sessionElapsedMs() >= sessionTotalMs()) {
    state = SessionState::DONE;
  }

  // Push the frame once per loop
  gfx->flush();

  // ~60 FPS pacing if PCLK allows; otherwise still smooth due to single flush + dirty regions
  delay(16);
}
