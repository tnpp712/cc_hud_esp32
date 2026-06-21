// led_ring.cpp — WS2812B 8-pixel status ring driver.
// See led_ring.h for the LED-pattern -> app-state mapping.

#include "led_ring.h"

#include <Adafruit_NeoPixel.h>

namespace cc_hud {

namespace {

// Most WS2812B strips ship as GRB at 800kHz.
Adafruit_NeoPixel g_ring(kLedRingCount, kPinLedRing,
                         NEO_GRB + NEO_KHZ800);

LedMode  g_mode      = kLedModeOff;
uint32_t g_last_step = 0;     // millis() of the last animation step
uint8_t  g_chase_pos = 0;     // leading-pixel index for the yellow comet
bool     g_blink_on  = false; // current half-period for the red blink
uint8_t  g_gauge_pct = 0;     // last quota % rendered by the gauge
uint8_t  g_brightness = kLedRingBrightness;  // night-dim scaled brightness
uint32_t g_pulse_until = 0;   // while millis() < this, the done-pulse owns the ring

constexpr uint32_t kPulseMs = 1800;   // total done-pulse duration

// Tier colour for the ambient gauge, mirrors the screen's tierColor().
uint32_t gaugeColor(uint8_t pct) {
    if (pct <= kThreshGreenMax)  return g_ring.Color(0, 255, 0);
    if (pct <= kThreshYellowMax) return g_ring.Color(255, 255, 0);
    return g_ring.Color(255, 0, 0);
}

// Render the gauge: ceil(pct/100 * count) pixels lit in tier colour, the
// rest dim. Always at least 1 lit so an idle low-usage ring isn't dark.
void renderGauge() {
    g_ring.clear();
    const uint32_t col = gaugeColor(g_gauge_pct);
    uint16_t lit = (static_cast<uint32_t>(g_gauge_pct) * kLedRingCount + 99) / 100;
    if (lit == 0) lit = 1;
    if (lit > kLedRingCount) lit = kLedRingCount;
    for (uint16_t i = 0; i < kLedRingCount; i++) {
        g_ring.setPixelColor(i, i < lit ? col : g_ring.Color(2, 2, 2));
    }
    g_ring.show();
}

}  // namespace

void ledRingInit() {
    g_ring.begin();
    g_ring.setBrightness(g_brightness);
    g_ring.clear();
    g_ring.show();
}

void ledRingSetBrightness(uint8_t brightness) {
    if (brightness == g_brightness) return;
    g_brightness = brightness;
    g_ring.setBrightness(brightness);
    // Re-show so the new brightness takes effect on the current frame.
    if (g_mode == kLedModeGauge) renderGauge();
    else                         g_ring.show();
}

void ledRingPulseDone() {
    g_pulse_until = millis() + kPulseMs;
    g_mode = kLedModeOff;   // force a re-assert after the pulse expires
}

void ledRingShowGauge(uint8_t pct) {
    if (millis() < g_pulse_until) return;   // done-pulse owns the ring
    if (pct > 100) pct = 100;
    if (g_mode == kLedModeGauge && pct == g_gauge_pct) return;
    g_mode      = kLedModeGauge;
    g_gauge_pct = pct;
    renderGauge();
}

void ledRingSetMode(LedMode mode) {
    if (millis() < g_pulse_until) return;   // done-pulse owns the ring
    if (mode == g_mode) return;
    g_mode      = mode;
    g_last_step = millis();
    g_chase_pos = 0;
    g_blink_on  = true;

    // Paint the static / first-frame state immediately so a mode change
    // is visible without waiting for the next tick.
    g_ring.clear();
    switch (mode) {
        case kLedModeIdleDone: {
            const uint32_t green = g_ring.Color(0, 255, 0);
            for (uint16_t i = 0; i < kLedRingCount; i++) {
                g_ring.setPixelColor(i, green);
            }
            break;
        }
        case kLedModeWaiting: {
            const uint32_t red = g_ring.Color(255, 0, 0);
            for (uint16_t i = 0; i < kLedRingCount; i++) {
                g_ring.setPixelColor(i, red);
            }
            break;
        }
        case kLedModeWorking:
            // Comet — first frame is the head at position 0.
            g_ring.setPixelColor(0, g_ring.Color(255, 255, 0));
            break;
        case kLedModeBattLow: {
            const uint32_t orange = g_ring.Color(255, 90, 0);
            for (uint16_t i = 0; i < kLedRingCount; i++) {
                g_ring.setPixelColor(i, orange);
            }
            break;
        }
        case kLedModeOff:
        default:
            break;  // already cleared
    }
    g_ring.show();
}

void ledRingTick() {
    const uint32_t now = millis();

    // Done-pulse: green triple-blink (~150ms on/off) for kPulseMs, on top of
    // whatever mode is active. When it expires, main re-asserts the normal
    // mode on the next loop (we cleared g_mode in ledRingPulseDone()).
    if (now < g_pulse_until) {
        const uint32_t since = kPulseMs - (g_pulse_until - now);  // 0..kPulseMs
        const bool on = ((since / 150u) & 1u) == 0u;              // 150ms phases
        const uint32_t c = on ? g_ring.Color(0, 255, 0) : 0;
        for (uint16_t i = 0; i < kLedRingCount; i++) g_ring.setPixelColor(i, c);
        g_ring.show();
        return;
    }
    switch (g_mode) {
        case kLedModeWorking: {
            if (now - g_last_step < kLedChaseStepMs) return;
            g_last_step = now;
            g_chase_pos = (g_chase_pos + 1) % kLedRingCount;
            g_ring.clear();
            // Comet: bright leader + a gradient tail, yellow. A longer tail
            // (kLedCometLen of kLedRingCount) reads as a smooth rotation
            // rather than a few sparse blinking dots.
            for (uint8_t i = 0; i < kLedCometLen && i < kLedRingCount; i++) {
                const uint8_t idx =
                    (g_chase_pos + kLedRingCount - i) % kLedRingCount;
                // Gentle falloff 255 → ~130 across the tail. A shallow
                // gradient keeps the whole tail visible even at low global
                // brightness, so "working" reads as a moving arc rather than
                // a single bright dot with an invisible trail.
                const uint8_t v = static_cast<uint8_t>(
                    255 - (i * 125) / (kLedCometLen - 1));
                g_ring.setPixelColor(idx, g_ring.Color(v, v, 0));
            }
            g_ring.show();
            break;
        }
        case kLedModeWaiting: {
            if (now - g_last_step < kLedBlinkHalfMs) return;
            g_last_step = now;
            g_blink_on  = !g_blink_on;
            const uint32_t c = g_blink_on ? g_ring.Color(255, 0, 0) : 0;
            for (uint16_t i = 0; i < kLedRingCount; i++) {
                g_ring.setPixelColor(i, c);
            }
            g_ring.show();
            break;
        }
        case kLedModeBattLow: {
            // Slow orange blink (1s half-period) — distinct from the faster
            // red waiting blink.
            if (now - g_last_step < kLedBlinkHalfMs * 2) return;
            g_last_step = now;
            g_blink_on  = !g_blink_on;
            const uint32_t c = g_blink_on ? g_ring.Color(255, 90, 0) : 0;
            for (uint16_t i = 0; i < kLedRingCount; i++) {
                g_ring.setPixelColor(i, c);
            }
            g_ring.show();
            break;
        }
        case kLedModeIdleDone:
        case kLedModeOff:
        default:
            // Static modes — nothing to advance.
            break;
    }
}

LedMode ledRingModeForAppState(int8_t app_state) {
    switch (app_state) {
        case kAppStateWaiting:  return kLedModeWaiting;
        case kAppStateTool:     return kLedModeWorking;
        case kAppStateThinking: return kLedModeWorking;
        case kAppStateIdle:     return kLedModeIdleDone;
        case kAppStateUnset:
        default:                return kLedModeOff;
    }
}

}  // namespace cc_hud
