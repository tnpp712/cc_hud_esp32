/**
 * lv_conf.h — minimal LVGL 9 configuration for cc_hud.
 *
 * Only overrides are listed; everything else falls back to the
 * defaults in lvgl/src/lv_conf_internal.h. Picked up via the
 * -DLV_CONF_INCLUDE_SIMPLE build flag (this folder is on the
 * include path automatically under PlatformIO).
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* RGB565 — matches the ST7789 panel and our existing assets. */
#define LV_COLOR_DEPTH 16

/* LVGL-internal heap for objects/styles. Static array in SRAM.
 * 48 KB is generous for 2 screens of widgets; we have ~280 KB free. */
#define LV_MEM_SIZE (48 * 1024U)

/* Target a 60 Hz refresh cadence (default is 33 ms ≈ 30 Hz). The
 * actual rate is bounded by how fast our flush callback pushes pixels
 * over the 40 MHz SPI — the spike's FPS label tells us the truth. */
#define LV_DEF_REFR_PERIOD 16

/* No OS — bare Arduino loop drives lv_timer_handler(). */
#define LV_USE_OS LV_OS_NONE

/* Logging off in production builds; flip to 1 + warn level if the
 * spike misbehaves. */
#define LV_USE_LOG 0

/* Fonts. 14 footer/date · 18 row labels + countdown sublabels ·
 * 20 plan title · 24 percent numbers · 48 idle clock. Each size is
 * roughly 15-20 KB flash; all five fit comfortably at 33% usage. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_48 1

#endif /* LV_CONF_H */
