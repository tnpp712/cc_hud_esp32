// button.cpp — debounced push button with short/long classification.
// See button.h.

#include "button.h"

#include "config.h"

namespace cc_hud {

namespace {

bool     g_raw_prev    = true;   // pull-up idle = HIGH = released
bool     g_pressed     = false;  // debounced state
uint64_t g_edge_ms     = 0;      // last raw transition time
uint64_t g_press_start = 0;      // when the (debounced) press began
bool     g_long_fired  = false;  // long event already emitted this hold

}  // namespace

void buttonInit() {
    pinMode(kPinButton, INPUT_PULLUP);
}

ButtonEvent buttonPoll(uint64_t now_ms) {
    const bool raw = (digitalRead(kPinButton) == LOW);  // LOW = pressed

    // Debounce: only accept a level after it's been stable kButtonDebounceMs.
    if (raw != g_raw_prev) {
        g_raw_prev = raw;
        g_edge_ms  = now_ms;
        return kBtnNone;
    }
    if (now_ms - g_edge_ms < kButtonDebounceMs) return kBtnNone;

    ButtonEvent ev = kBtnNone;
    if (raw && !g_pressed) {
        // Debounced press begins.
        g_pressed     = true;
        g_press_start = now_ms;
        g_long_fired  = false;
    } else if (raw && g_pressed && !g_long_fired) {
        // Still held — fire long press once the threshold is crossed.
        if (now_ms - g_press_start >= kButtonLongPressMs) {
            g_long_fired = true;
            ev = kBtnLong;
        }
    } else if (!raw && g_pressed) {
        // Released. If we never fired long, it was a short press.
        g_pressed = false;
        if (!g_long_fired) ev = kBtnShort;
    }
    return ev;
}

}  // namespace cc_hud
