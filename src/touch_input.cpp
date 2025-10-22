// src/touch_input.cpp
#include "touch_input.h"
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <Adafruit_FT6206.h>   // FT6x36 family

// --- Optional GT911 library; compile even if it's missing ---
#if __has_include(<GT911.h>)
  #include <GT911.h>
  #define HAVE_GT911_LIB 1
#else
  #define HAVE_GT911_LIB 0
#endif

// ---------------- Internals ----------------
enum class TouchIC : uint8_t { NONE=0, FT6X36, GT911 };
static TouchIC s_ic = TouchIC::NONE;
static uint8_t s_addr = 0x00;

static Adafruit_FT6206 s_ft;

#if HAVE_GT911_LIB
  static GT911 s_gt;
#endif
static bool s_gt_using_lib = false;
static lv_indev_t* s_indev = nullptr;

// --- Screen geometry (from your platformio.ini -D’s) ---
#ifndef TOUCH_SCREEN_W
#define TOUCH_SCREEN_W 800
#endif
#ifndef TOUCH_SCREEN_H
#define TOUCH_SCREEN_H 480
#endif

// GT911 minimal raw map
static constexpr uint16_t GT_REG_PRODUCT_ID = 0x8140; // 4 bytes ASCII
static constexpr uint16_t GT_REG_STATUS     = 0x814E; // [7]=buf ready, [3:0]=points
static constexpr uint16_t GT_REG_POINTS     = 0x8150; // first point block

struct GTPointRaw {
  uint16_t x;
  uint16_t y;
  uint16_t size;
  uint8_t  id;
  uint8_t  reserved;
} __attribute__((packed));

static inline void orient_map(int16_t& x, int16_t& y) {
#if TOUCH_SWAP_XY
  int16_t t=x; x=y; y=t;
#endif
#if TOUCH_INVERT_X
  x = TOUCH_SCREEN_W - 1 - x;
#endif
#if TOUCH_INVERT_Y
  y = TOUCH_SCREEN_H - 1 - y;
#endif
  if (x < 0) x = 0; if (y < 0) y = 0;
  if (x >= TOUCH_SCREEN_W) x = TOUCH_SCREEN_W - 1;
  if (y >= TOUCH_SCREEN_H) y = TOUCH_SCREEN_H - 1;
}

// -------- I2C helpers --------
static bool i2c_probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}
static bool i2c_write_u8(uint8_t addr, uint16_t reg, uint8_t v) {
  uint8_t pkt[3] = { uint8_t(reg>>8), uint8_t(reg&0xFF), v };
  Wire.beginTransmission(addr);
  Wire.write(pkt, 3);
  return (Wire.endTransmission() == 0);
}
static bool i2c_read(uint8_t addr, const uint8_t* reg, size_t reg_len, uint8_t* buf, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg, reg_len);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)len) != (int)len) return false;
  for (size_t i=0;i<len;i++) buf[i] = Wire.read();
  return true;
}

// -------- Bus recovery (if SDA stuck low) --------
bool i2c_bus_recover(uint8_t sclPin, uint8_t sdaPin) {
  Wire.end();
  pinMode(sclPin, INPUT_PULLUP);
  pinMode(sdaPin, INPUT_PULLUP);
  delay(1);

  if (digitalRead(sdaPin) == HIGH) {
    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    Wire.setClock(100000);
    return true;
  }

  // SDA low: clock SCL to free it
  pinMode(sclPin, OUTPUT);
  for (int i=0; i<16 && (digitalRead(sdaPin)==LOW); ++i) {
    digitalWrite(sclPin, HIGH); delayMicroseconds(5);
    digitalWrite(sclPin, LOW ); delayMicroseconds(5);
  }

  // STOP condition
  pinMode(sdaPin, OUTPUT);
  digitalWrite(sdaPin, LOW);  delayMicroseconds(5);
  digitalWrite(sclPin, HIGH); delayMicroseconds(5);
  digitalWrite(sdaPin, HIGH); delayMicroseconds(5);

  Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
  Wire.setClock(100000);
  delay(3);
  return (digitalRead(sdaPin) == HIGH);
}

void i2c_full_scan_print(Stream& out) {
  out.print(F("I2C scan:"));
  bool any = false;
  for (uint8_t a=1; a<127; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { out.printf(" 0x%02X", a); any = true; }
  }
  if (!any) out.print(F(" (none)"));
  out.println();
}

// -------- Reset/INT helpful sequence (if wired) --------
static void touch_hw_reset_sequence() {
  if (TOUCH_RST_PIN >= 0) {
    pinMode(TOUCH_RST_PIN, OUTPUT);
    digitalWrite(TOUCH_RST_PIN, HIGH);
  }
  if (TOUCH_INT_PIN >= 0) {
    pinMode(TOUCH_INT_PIN, INPUT_PULLUP);
  }

  // Simple GT911-friendly reset pulse; harmless for FT6x36
  if (TOUCH_RST_PIN >= 0) {
    if (TOUCH_INT_PIN >= 0) { pinMode(TOUCH_INT_PIN, OUTPUT); digitalWrite(TOUCH_INT_PIN, LOW); }
    digitalWrite(TOUCH_RST_PIN, LOW);  delay(10);
    digitalWrite(TOUCH_RST_PIN, HIGH); delay(10);
    if (TOUCH_INT_PIN >= 0) pinMode(TOUCH_INT_PIN, INPUT_PULLUP);
  }
}

// -------- Detection --------
static void detect_ic() {
  touch_hw_reset_sequence();

  bool has_ft   = i2c_probe(0x38);
  bool has_gt5d = i2c_probe(0x5D);
  bool has_gt14 = i2c_probe(0x14);

  Serial.printf("[touch][probe] 0x38=%d 0x5D=%d 0x14=%d\n", has_ft, has_gt5d, has_gt14);

  if (has_ft) {
    // Extra sanity: some boards pull 0x38 but aren’t FT; quick ID read (best-effort)
    uint8_t chip = 0, vend = 0;
    uint8_t regc[1] = { 0xA8 }; // CHIPID
    uint8_t regv[1] = { 0xA3 }; // VENDID
    if (i2c_read(0x38, regc, 1, &chip, 1) && i2c_read(0x38, regv, 1, &vend, 1)) {
      Serial.printf("[touch][FT] CHIPID=0x%02X VENDID=0x%02X\n", chip, vend);
      if (chip == 0x06 || chip == 0x36) {
        s_ic = TouchIC::FT6X36; s_addr = 0x38;
        Serial.println(F("[touch] FT6x36 detected @ 0x38"));
        return;
      }
    }
    Serial.println(F("[touch][FT] 0x38 ACKed but IDs not FT → ignoring"));
  }

  if (has_gt5d || has_gt14) {
    s_ic = TouchIC::GT911; s_addr = has_gt5d ? 0x5D : 0x14;
    Serial.printf("[touch] GT911 selected @ 0x%02X\n", s_addr);

    // Product ID read
    uint8_t idbuf[4] = {0};
    uint8_t reg[2] = { uint8_t(GT_REG_PRODUCT_ID>>8), uint8_t(GT_REG_PRODUCT_ID & 0xFF) };
    if (i2c_read(s_addr, reg, 2, idbuf, 4)) {
      Serial.printf("[touch][GT] Product ID: %c%c%c%c\n", idbuf[0], idbuf[1], idbuf[2], idbuf[3]);
    }

    // Resolution read (0x8048 little-endian X, 0x804A little-endian Y)
    uint8_t cfg_reg[2] = { 0x80, 0x47 };
    uint8_t cfg[8] = {0};
    if (i2c_read(s_addr, cfg_reg, 2, cfg, 7)) {
      uint16_t x = (uint16_t)cfg[2] | ((uint16_t)cfg[3] << 8);
      uint16_t y = (uint16_t)cfg[4] | ((uint16_t)cfg[5] << 8);
      Serial.printf("[touch][GT] cfg: %u x %u\n", x, y);
    }
    return;
  }

  s_ic = TouchIC::NONE; s_addr = 0x00;
  Serial.println(F("[touch] No FT/GT touch IC found (0x38/0x5D/0x14)"));
}

// -------- LVGL read cb --------
static void touch_read_cb(lv_indev_t*, lv_indev_data_t* data) {
  data->continue_reading = false;
  data->point.x = 0; data->point.y = 0;
  data->state = LV_INDEV_STATE_RELEASED;

  if (s_ic == TouchIC::FT6X36) {
    if (s_ft.touched()) {
      TS_Point p = s_ft.getPoint();
      int16_t x = p.x, y = p.y;
      orient_map(x,y);
      data->point.x = x; data->point.y = y;
      data->state = LV_INDEV_STATE_PRESSED;
    }
    return;
  }

  if (s_ic == TouchIC::GT911) {
    bool pressed = false; int16_t x=0, y=0;

#if HAVE_GT911_LIB
    if (s_gt_using_lib) {
      s_gt.read();
      if (s_gt.isTouched()) {
        auto pt = s_gt.getPoint(0);
        x = pt.x; y = pt.y; pressed = true;
      }
    } else
#endif
    {
      // raw minimal read (with robust ack/clear)
      uint8_t status = 0;
      uint8_t regS[2] = { uint8_t(GT_REG_STATUS>>8), uint8_t(GT_REG_STATUS & 0xFF) };
      if (i2c_read(s_addr, regS, 2, &status, 1)) {
        uint8_t n = status & 0x0F;
        bool buf_ready = status & 0x80;

        if (buf_ready) {
          // Try read first point anyway (some firmwares set buf_ready early)
          uint8_t regP[2] = { uint8_t(GT_REG_POINTS>>8), uint8_t(GT_REG_POINTS & 0xFF) };
          GTPointRaw pr{};
          if (i2c_read(s_addr, regP, 2, (uint8_t*)&pr, sizeof(pr))) {
            // Only trust if coords look sane and n>0
            if (n > 0 && pr.x != 0xFFFF && pr.y != 0xFFFF) {
              x = (int16_t)pr.x; y = (int16_t)pr.y; pressed = true;
            }
            // Debug peek (once in a while)
            static uint32_t last_dump = 0;
            uint32_t now = millis();
            if (now - last_dump > 500) {
              Serial.printf("[touch][GT] status=0x%02X n=%u peek: x=%u y=%u id=%u size=%u\n",
                            status, n, pr.x, pr.y, pr.id, pr.size);
              last_dump = now;
            }
          }

          // ACK/CLEAR the buffer-ready flag ALWAYS, even if n==0
          (void)i2c_write_u8(s_addr, GT_REG_STATUS, 0x00);
        }
      }
    }

    if (pressed) {
      orient_map(x,y);
      data->point.x = x; data->point.y = y;
      data->state = LV_INDEV_STATE_PRESSED;
    }
  }
}

// -------- Public API --------
void touch_init_and_register_lvgl() {
  // Ensure Wire is alive and bus released
  Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
  Wire.setClock(100000);
  delay(3);
  (void)i2c_bus_recover(); // harmless if bus already free
  i2c_full_scan_print(Serial);   // log addresses

  detect_ic();

  if (s_ic == TouchIC::FT6X36) {
    // Adafruit FT6206 begin() only accepts a threshold; it uses default Wire
    if (!s_ft.begin(30)) {
      Serial.println(F("[touch] FT6x36 begin() failed"));
      s_ic = TouchIC::NONE;
    } else {
      Serial.println(F("[touch] FT6x36 ready"));
    }
  } else if (s_ic == TouchIC::GT911) {
#if HAVE_GT911_LIB
    s_gt_using_lib = s_gt.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL, s_addr, TOUCH_RST_PIN, TOUCH_INT_PIN);
    if (s_gt_using_lib) Serial.println(F("[touch] GT911 library initialized"));
    else                Serial.println(F("[touch] GT911 lib init failed; using raw I2C"));
#else
    s_gt_using_lib = false;
    Serial.println(F("[touch] GT911 lib not present; using raw I2C"));
#endif
  }

  if (s_ic != TouchIC::NONE) {
    // Speed up after detection
    Wire.setClock(400000);
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);
    Serial.printf("[touch] LVGL indev registered (%s @ 0x%02X)\n",
                  (s_ic==TouchIC::FT6X36 ? "FT6x36":"GT911"), s_addr);
  } else {
    Serial.println(F("[touch] Skipping LVGL indev (no touch detected)"));
  }
}

bool touch_present() { return s_ic != TouchIC::NONE; }
const char* touch_ic_name() {
  switch (s_ic) { case TouchIC::FT6X36: return "FT6x36";
                  case TouchIC::GT911:  return "GT911";
                  default:              return "NONE"; }
}
uint8_t touch_i2c_address() { return s_addr; }
