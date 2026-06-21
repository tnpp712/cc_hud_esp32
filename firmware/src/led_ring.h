// led_ring.h
// 8-pixel WS2812B status ring. Drives one of three patterns based on the
// Claude Code app-state pushed over BLE:
//   - Working (Tool / Thinking) → yellow comet rotating once / 0.8s
//   - Waiting (notification, permission prompt) → red full-ring blink 1Hz
//   - Idle (Stop hook — turn finished) → green steady
//   - Unset / long AFK → all off
//
// Brightness is intentionally capped (kLedRingBrightness in config.h) — the
// ring runs straight off the 3.7V lithium and 8 LEDs at full brightness
// would blow the battery budget for HUD use.

#pragma once

#include "config.h"

namespace cc_hud {

enum LedMode : uint8_t {
    kLedModeOff      = 0,   // all pixels off
    kLedModeIdleDone = 1,   // green steady (Claude finished its turn)
    kLedModeWorking  = 2,   // yellow comet chase (Tool / Thinking)
    kLedModeWaiting  = 3,   // red full-ring blink (Notification)
    kLedModeGauge    = 4,   // ambient quota gauge: N/8 pixels lit, tiered color
    kLedModeBattLow  = 5,   // slow orange blink — battery low (when wired)
};

void ledRingInit();
void ledRingSetMode(LedMode mode);
void ledRingTick();   // advance animation; call every loop()

// Ambient quota gauge: light a proportional arc (pct of the ring) coloured
// green/yellow/red by tier. Use this in place of kLedModeIdleDone so the
// ring is glanceable as a quota meter whenever Claude is idle.
void ledRingShowGauge(uint8_t pct);

// One-shot "an AI finished a turn" pulse — a brief green triple-blink that
// overrides the ring for ~1.8s then yields back to the normal state. Used
// for tools (e.g. Codex) that only expose a turn-complete event, so there's
// a distinct, glanceable "done — your move" signal.
void ledRingPulseDone();

// Runtime brightness scale 0..255 (night auto-dim). Applied on top of the
// per-pattern colours. 0 = effectively off, 255 = full kLedRingBrightness.
void ledRingSetBrightness(uint8_t brightness);

// Translate AppState (config.h) -> LedMode. Tool and Thinking both map
// to Working — the screen already differentiates them in detail; the LED
// only needs to convey "active vs waiting vs done".
LedMode ledRingModeForAppState(int8_t app_state);

}  // namespace cc_hud
