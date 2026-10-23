#pragma once
#include <cstdint>
namespace cc_hud {
// 输入为已用百分比，亮灯颗数表达剩余额度；耗尽时全部熄灭。
constexpr uint8_t remainingPixels(uint8_t used, uint8_t count) {
    return static_cast<uint8_t>(((100u - (used > 100 ? 100u : used)) * count + 99u) / 100u);
}
}
