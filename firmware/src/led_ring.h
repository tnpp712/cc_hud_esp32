// 状态灯环：工作柔和青绿，待处理琥珀呼吸，明确错误红闪，结束单次绿脉冲。

#pragma once

#include "config.h"
#include "led_policy.h"

namespace cc_hud {

void ledRingInit();
void ledRingSetMode(LedMode mode);
void ledRingTick();   // advance animation; call every loop()

// Ambient quota gauge: light a proportional arc (pct of the ring) coloured
// green/yellow/red by tier. Use this in place of kLedModeIdleDone so the
// ring is glanceable as a quota meter whenever Claude is idle.
void ledRingShowGauge(uint8_t pct);

// 本轮结束后单次柔和绿脉冲，约 1.2 秒；待处理和错误优先。
void ledRingPulseDone();

// Runtime brightness scale 0..255 (night auto-dim). Applied on top of the
// per-pattern colours. 0 = effectively off, 255 = full kLedRingBrightness.
void ledRingSetBrightness(uint8_t brightness);

// Translate AppState (config.h) -> LedMode. Tool and Thinking both map
// to Working — the screen already differentiates them in detail; the LED
// only needs to convey "active vs waiting vs done".

}  // namespace cc_hud
