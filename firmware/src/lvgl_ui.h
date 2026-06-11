// lvgl_ui.h
// LVGL 9 user interface for cc_hud — replaces the legacy hand-drawn
// renderer for the HUD and idle screens. The OTA progress screen stays
// on the legacy Adafruit path in display.cpp (LVGL stops ticking while
// OTA owns the panel; the device reboots after).
//
// Data flow: main.cpp stays the owner of all state. Once per loop it
// fills an LvglUiModel and calls lvglUiApply(); widget updates happen
// only where values actually changed. lvglUiTick() pumps LVGL timers.

#pragma once

#if CCHUD_LVGL_UI

#include <Arduino.h>

#include "config.h"
#include "persistence.h"

namespace cc_hud {

struct LvglUiModel {
    QuotaSnapshot quota;          // latest quota + idle-time fields
    bool          ble_connected = false;
    bool          idle_mode     = false;   // true → clock screen
    AppState      app_state     = kAppStateUnset;
    char          app_detail[kAppStateDetailMaxLen + 1] = {0};
    PetMood       mood          = kPetMoodHappy;
    uint64_t      now_ms        = 0;
};

// Bring up LVGL (tick source, display driver, both screens, assets).
// Call once from setup(), after displayInit().
void lvglUiInit();

// Pump LVGL timers/refresh. Call every loop() iteration.
void lvglUiTick();

// Diff the model against the on-screen state and update widgets.
// Cheap when nothing changed; call every loop() iteration.
void lvglUiApply(const LvglUiModel& m);

// One-shot red alert flash (non-blocking; ~5 s of pulsing overlay).
void lvglUiFlashAlert();

}  // namespace cc_hud

#endif  // CCHUD_LVGL_UI
