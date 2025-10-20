#include <Arduino.h>
#include <Wire.h>

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
  /* pclk_neg */ 1,  /* prefer speed */ 16000000   // Try 32000000 for ~60 Hz
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
static bool  paused = false;

// Computed each frame
static float fLeft = 0.0f, fRight = 0.0f;

// Dirty-state caches (to minimize redraw)
static float lastBase = NAN, lastBeat = NAN, lastLeft = NAN, lastRight = NAN;
static int   lastPreset = -1;
static bool  lastPaused = true;
static int   lastVizX   = -1;

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
    Wire.begin(I2C_SDA, I2C_SCL);                 // attach to the pins we’re using
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

void handleSerial()
{
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case '1': applyPreset(0); break;
      case '2': applyPreset(1); break;
      case '3': applyPreset(2); break;
      case '4': applyPreset(3); break;
      case 'q': baseHz += 5.0f; break;
      case 'a': baseHz -= 5.0f; if (baseHz < 20.0f) baseHz = 20.0f; break;
      case 'w': beatHz += 0.5f; break;
      case 's': beatHz -= 0.5f; break;
      case 'r': baseHz = PRESETS[presetIdx].baseHz; beatHz = PRESETS[presetIdx].beatHz; break;
      case 'p': paused = !paused; break;
      default: break;
    }
  }
}

// ==================== Drawing (dirty-region updates) ====================
void drawStaticOnce()
{
  // Header bar bg (text drawn in drawHeaderIfChanged)
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

  // Static footer hint area (will be overwritten as needed)
  gfx->fillRect(40, 260, 720, 90, BLACK);
}

void drawHeaderIfChanged()
{
  if (lastPreset!=presetIdx || lastBase!=baseHz || lastBeat!=beatHz || lastPaused!=paused) {
    gfx->fillRect(0, 0, 800, 42, 0x0841);
    gfx->setTextColor(WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(12, 12);
    gfx->printf("Preset: %s   Base: %.1f Hz   Beat: %.2f Hz   %s",
                PRESETS[presetIdx].name, baseHz, beatHz, paused ? "[PAUSED]" : " ");
    lastPreset = presetIdx; lastPaused = paused;
  }
}

void drawFreqsIfChanged()
{
  // Left number area
  if (lastLeft!=fLeft) {
    gfx->fillRect(60, 145, 260, 40, 0x39E7); // clear number area only
    gfx->setTextColor(WHITE); gfx->setTextSize(3); gfx->setCursor(60, 150);
    gfx->printf("%.2f", fLeft);
    lastLeft = fLeft;
  }
  // Right number area
  if (lastRight!=fRight) {
    gfx->fillRect(460, 145, 260, 40, 0x39E7);
    gfx->setTextColor(WHITE); gfx->setTextSize(3); gfx->setCursor(460, 150);
    gfx->printf("%.2f", fRight);
    lastRight = fRight;
  }

  // Info lines
  if (lastBase!=baseHz || lastBeat!=beatHz) {
    gfx->fillRect(40, 260, 720, 90, BLACK);
    gfx->setTextColor(WHITE); gfx->setTextSize(2);
    gfx->setCursor(40, 260); gfx->printf("Carrier/Base: %.2f Hz", baseHz);
    gfx->setCursor(40, 290); gfx->printf("Beat Delta:   %.2f Hz  (L = base - beat/2, R = base + beat/2)", beatHz);
    gfx->setCursor(40, 320); gfx->print("Serial: 1/2/3/4 presets | q/a base +/-5 | w/s beat +/-0.5 | r reset | p pause");
    lastBase = baseHz; lastBeat = beatHz;
  }
}

void drawVisualizerThin(float t)
{
  // Map beatHz 0.5..30 to a movement speed
  float speed = beatHz;
  if (speed < 0.5f) speed = 0.5f;
  if (speed > 30.0f) speed = 30.0f;
  speed /= 30.0f; // 0..1

  int x = 40 + (int)(sin(t * speed * 2.0f * PI) * 200.0f + 200.0f);

  // Erase previous segment only
  if (lastVizX >= 0) {
    gfx->fillRect(40, 340, lastVizX, 12, 0x10A2); // back to baseline color
  }
  // Draw new segment
  gfx->fillRect(40, 340, x, 12, 0xFBE0);
  lastVizX = x;
}

// ==================== Arduino lifecycle ====================
void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\n--- Binaural UI (Dirty-Region Version) ---");
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

  // Push the composed frame
  gfx->flush();

  Serial.println("[OK] Ready. Controls: 1/2/3/4 presets | q/a base +/-5 | w/s beat +/-0.5 | r reset | p pause");
}

void loop()
{
  handleSerial();

  // Only update when not paused
  if (!paused) {
    computeEngine();

    // Header / numeric readouts update if changed
    drawHeaderIfChanged();
    drawFreqsIfChanged();

    // Simple animated visualizer
    static uint32_t t0 = millis();
    float t = (millis() - t0) / 1000.0f;
    drawVisualizerThin(t);
  }

  // Heartbeat pixel (top-left)
  static bool on = false;
  gfx->drawPixel(0, 0, on ? GREEN : RED);
  on = !on;

  // Push the frame once per loop
  gfx->flush();

  // ~60 FPS pacing if PCLK allows; otherwise still smoother (single flush) at lower PCLK
  delay(16);
}
