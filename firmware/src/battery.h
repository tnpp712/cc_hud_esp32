// battery.h
// Optional Li-ion battery voltage monitor on an ADC1 pin (kPinBatterySense).
// Ships safe when unwired: a floating pin reads an implausible voltage, so
// batteryValid() returns false and the UI simply hides the battery info.
//
// Wiring (2:1 divider so the 4.2V max stays within the ADC range):
//   BAT+ ── 100kΩ ──┬── GPIO1
//                   └── 100kΩ ── GND

#pragma once

#include <Arduino.h>

namespace cc_hud {

void     batteryInit();

// Sample the ADC and update the smoothed reading. Call periodically
// (e.g. once per second) — not every loop, ADC reads aren't free.
void     batteryTick(uint64_t now_ms);

// True when the last reading is a plausible Li-ion voltage (sensor wired).
bool     batteryValid();

// Smoothed battery voltage in millivolts (whole battery, divider undone).
uint16_t batteryMilliVolts();

// 0..100 battery percent (mapped from kBatEmptyMv..kBatFullMv). 255 if
// no valid reading.
uint8_t  batteryPercent();

// True when valid AND below the low-battery threshold.
bool     batteryLow();

}  // namespace cc_hud
