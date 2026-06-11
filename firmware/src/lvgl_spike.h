// lvgl_spike.h
// Phase-1 LVGL validation screen. Proves the LVGL 9 → Adafruit_ST7789
// flush path on this exact wiring before we commit to a full UI
// migration. Compiled in only when CCHUD_LVGL_SPIKE=1 (platformio.ini).

#pragma once

#if CCHUD_LVGL_SPIKE

namespace cc_hud {

// Bring up LVGL: tick source, display driver with two partial SRAM
// buffers, flush callback into the Adafruit panel, then build the
// test screen (animated bar + spinner + live FPS readout).
// Call once from setup(), after displayInit().
void lvglSpikeInit();

// Drive LVGL's timers/refresh. Call every loop() iteration; LVGL
// self-paces internally to LV_DEF_REFR_PERIOD.
void lvglSpikeTick();

}  // namespace cc_hud

#endif  // CCHUD_LVGL_SPIKE
