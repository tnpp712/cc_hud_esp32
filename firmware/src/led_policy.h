#pragma once
#include <cstdint>
namespace cc_hud {
// 状态编码遵循现有 BLE AppState 协议。
enum LedMode : uint8_t {
    kLedModeOff      = 0,   // all pixels off
    kLedModeIdleDone = 1,   // green steady (Claude finished its turn)
    kLedModeWorking  = 2,   // yellow comet chase (Tool / Thinking)
    kLedModeWaiting  = 3,   // 琥珀色呼吸，等待用户处理
    kLedModeGauge    = 4,   // ambient quota gauge: N/8 pixels lit, tiered color
    kLedModeError    = 6,   // 明确错误使用红色闪烁
    kLedModeBattLow  = 5,   // slow orange blink — battery low (when wired)
};

inline LedMode ledRingModeForAppState(int8_t state,uint8_t kind=0) {
    if(state==3)return kind==3?kLedModeError:kLedModeWaiting;
    if(state==2 || state==1)return kLedModeWorking;
    if(state==0)return kLedModeIdleDone;
    return kLedModeOff;
}

}
