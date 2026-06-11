// lvgl_spike.cpp
// LVGL 9 spike: display driver glue + a test screen that makes render
// throughput visible. What we want to learn:
//   1. does the flush path work on this wiring (colors, orientation)?
//   2. what's the real refresh rate at 40 MHz SPI with partial buffers?
//   3. how do LVGL's animations feel vs. our hand-rolled ticks?

#if CCHUD_LVGL_SPIKE

#include <Arduino.h>
#include <Adafruit_ST7789.h>
#include <lvgl.h>

#include "config.h"
#include "display.h"
#include "lvgl_spike.h"

namespace cc_hud {

namespace {

// Two partial draw buffers — 40 rows each, double-buffered so LVGL
// renders into one while the other is being flushed. 2 × 19.2 KB of
// static SRAM (PSRAM is disabled on this board; SRAM has ~280 KB free).
constexpr int kBufRows = 40;
alignas(4) uint8_t g_buf_a[kScreenWidth * kBufRows * 2];
alignas(4) uint8_t g_buf_b[kScreenWidth * kBufRows * 2];

// Flush statistics for the on-screen FPS label.
volatile uint32_t g_flushed_px = 0;

// LVGL → panel. px_map is RGB565 in native (little-endian) uint16_t
// order, which is exactly what Adafruit's drawRGBBitmap expects — it
// handles the MSB-first SPI byte order itself. If colors ever come out
// channel-swapped, insert lv_draw_sw_rgb565_swap() here.
void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    displayGetTft().drawRGBBitmap(area->x1, area->y1,
                                   reinterpret_cast<uint16_t*>(px_map),
                                   w, h);
    g_flushed_px += static_cast<uint32_t>(w) * h;
    lv_display_flush_ready(disp);
}

uint32_t tick_cb() {
    return millis();
}

// Test-screen widgets we update from timers.
lv_obj_t* g_fps_label = nullptr;
lv_obj_t* g_bar       = nullptr;

void bar_anim_cb(void* obj, int32_t v) {
    lv_bar_set_value(static_cast<lv_obj_t*>(obj), v, LV_ANIM_OFF);
}

// Once a second: convert flushed pixels to "full-frame equivalents/s".
// That's the honest throughput number — partial redraws mean LVGL can
// hit high effective rates even when full-screen sweeps would be slow.
void fps_timer_cb(lv_timer_t*) {
    static uint32_t last_ms = 0;
    const uint32_t now = millis();
    const uint32_t dt  = now - last_ms;
    last_ms = now;
    const uint32_t px  = g_flushed_px;
    g_flushed_px = 0;
    if (dt == 0) return;
    const float frames = static_cast<float>(px) /
                         (kScreenWidth * kScreenHeight);
    const float fps    = frames * 1000.0f / dt;
    lv_label_set_text_fmt(g_fps_label, "%.1f frame/s", fps);
}

void buildTestScreen() {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Claude-orange title.
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "LVGL 9 spike");
    lv_obj_set_style_text_color(title, lv_color_hex(0xD77757), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    // Animated bar — sweeps 0..100 and back, forever.
    g_bar = lv_bar_create(scr);
    lv_obj_set_size(g_bar, 190, 16);
    lv_obj_align(g_bar, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(g_bar, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bar, lv_color_hex(0xD77757), LV_PART_INDICATOR);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_bar);
    lv_anim_set_exec_cb(&a, bar_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_playback_duration(&a, 1200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    // Spinner — continuous arc rotation, the classic smoothness probe.
    lv_obj_t* spinner = lv_spinner_create(scr);
    lv_obj_set_size(spinner, 56, 56);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 48);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0xD77757), LV_PART_INDICATOR);

    // Live FPS readout, bottom.
    g_fps_label = lv_label_create(scr);
    lv_label_set_text(g_fps_label, "-- frame/s");
    lv_obj_set_style_text_color(g_fps_label, lv_color_hex(0x9CA3AF), 0);
    lv_obj_set_style_text_font(g_fps_label, &lv_font_montserrat_14, 0);
    lv_obj_align(g_fps_label, LV_ALIGN_BOTTOM_MID, 0, -12);

    lv_timer_create(fps_timer_cb, 1000, nullptr);
}

}  // namespace

void lvglSpikeInit() {
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t* disp = lv_display_create(kScreenWidth, kScreenHeight);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, g_buf_a, g_buf_b, sizeof(g_buf_a),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    buildTestScreen();
    Serial.println("[LVGL] spike up: 2x40-row SRAM buffers, partial mode");
}

void lvglSpikeTick() {
    lv_timer_handler();
}

}  // namespace cc_hud

#endif  // CCHUD_LVGL_SPIKE
