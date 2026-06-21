// button.h
// Optional momentary push button on kPinButton (to GND, internal pull-up).
// Ships safe when unwired: pull-up keeps the pin HIGH (released).
//
// Wiring: GPIO2 ── button ── GND.
//
// Debounced edge detection with short/long press classification.

#pragma once

#include <Arduino.h>

namespace cc_hud {

enum ButtonEvent : uint8_t {
    kBtnNone  = 0,
    kBtnShort = 1,   // press + release under kButtonLongPressMs
    kBtnLong  = 2,   // held ≥ kButtonLongPressMs
};

void        buttonInit();

// Poll the pin; returns an event once per completed press. Call every loop.
ButtonEvent buttonPoll(uint64_t now_ms);

}  // namespace cc_hud
