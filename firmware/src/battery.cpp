// battery.cpp — Li-ion voltage monitor. See battery.h.

#include "battery.h"

#include "config.h"

namespace cc_hud {

namespace {

constexpr uint32_t kSampleIntervalMs = 1000;

bool     g_valid     = false;
uint16_t g_mv        = 0;        // smoothed whole-battery millivolts
uint64_t g_last_ms   = 0;
bool     g_have_ema  = false;

}  // namespace

void batteryInit() {
    // analogReadMilliVolts() handles attenuation + factory calibration on
    // ESP32-S3; default 11dB attenuation reads up to ~3.1-3.3V at the pin,
    // which (after the 2:1 divider) covers the full Li-ion range.
    analogReadResolution(12);
    // INPUT_PULLDOWN (not plain INPUT): when no divider is wired the pin is
    // pulled to ~0 → reads implausible → batteryValid() stays false. A
    // floating INPUT pin can drift to mid-rail and falsely report a battery.
    // With the 2.2k/2.2k divider wired, the ~45k internal pulldown shifts
    // the ratio by ~2% — negligible for a battery percentage.
    pinMode(kPinBatterySense, INPUT_PULLDOWN);
}

void batteryTick(uint64_t now_ms) {
    if (g_have_ema && now_ms - g_last_ms < kSampleIntervalMs) return;
    g_last_ms = now_ms;

    const uint32_t pin_mv = analogReadMilliVolts(kPinBatterySense);
    const uint32_t vbat   = pin_mv * kBatDividerRatioX10 / 10u;

    if (vbat < kBatPlausibleMinMv || vbat > kBatPlausibleMaxMv) {
        g_valid = false;          // floating pin / no sensor wired
        return;
    }
    // EMA smoothing to ride out ADC noise + load transients.
    if (!g_have_ema) { g_mv = vbat; g_have_ema = true; }
    else             { g_mv = (g_mv * 3u + vbat) / 4u; }
    g_valid = true;
}

bool batteryValid() { return g_valid; }

uint16_t batteryMilliVolts() { return g_mv; }

uint8_t batteryPercent() {
    if (!g_valid) return 255;
    if (g_mv >= kBatFullMv)  return 100;
    if (g_mv <= kBatEmptyMv) return 0;
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(g_mv - kBatEmptyMv) * 100u) /
        (kBatFullMv - kBatEmptyMv));
}

bool batteryLow() {
    return g_valid && g_mv < kBatLowMv;
}

}  // namespace cc_hud
