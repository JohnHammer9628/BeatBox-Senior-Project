/**
 * LVGL v9 config for ESP32-S3 + 800x480
 * Put this at: <project>/include/lv_conf.h
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* --- Logging (keep light) --- */
#define LV_USE_LOG               1
#define LV_LOG_LEVEL             LV_LOG_LEVEL_WARN
#pragma message(">>> USING PROJECT lv_conf.h <<<")


/* --- Color / display --- */
#define LV_COLOR_DEPTH           16
#define LV_COLOR_16_SWAP         0
#define LV_HOR_RES_MAX           800
#define LV_VER_RES_MAX           480

/* --- Draw engine --- */
#define LV_USE_DRAW_SW           1

/* --- Memory --- */
#define LV_USE_BUILTIN_MALLOC    1

/* --- Fonts: enable bigger Montserrat faces --- */
#define LV_FONT_MONTSERRAT_16    1
#define LV_FONT_MONTSERRAT_20    1
#define LV_FONT_MONTSERRAT_24    1

/* Default font used where we don't override */
#define LV_FONT_DEFAULT          &lv_font_montserrat_20

/* Keep other options default */
#endif /* LV_CONF_H */
