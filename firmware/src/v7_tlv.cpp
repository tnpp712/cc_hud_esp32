#include "v7_tlv.h"
#include <cstring>

namespace cc_hud {

// 从小端字节序读 u32
static uint32_t rdU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// 安全拷贝字符串,保证 null 终止
static void cpyStr(char* dst, size_t cap, const uint8_t* src, uint8_t len) {
    size_t n = len < cap - 1 ? len : cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

V7Result parseV7Tlv(const uint8_t* buf, size_t len, V7Fields& out) {
    // 帧头至少 4 字节:[msg_type, flags, seq, total]
    if (len < 4) return V7_ERR_LEN;
    // 本层只接受单片帧(total==1);分片需上层重组
    if (buf[3] != 1) return V7_ERR_FRAGMENT;

    size_t i = 4;
    while (i + 2 <= len) {
        uint8_t tag = buf[i];
        uint8_t l   = buf[i + 1];
        // 越界检测:value 区域必须完整在 buf 内
        if (i + 2 + l > len) return V7_ERR_LEN;
        const uint8_t* v = buf + i + 2;

        switch (tag) {
            case kV7TagFiveHUsedPct:
                if (l >= 1) { out.has_five_h_used = true; out.five_h_used = v[0]; }
                break;
            case kV7TagFiveHResetInS:
                if (l >= 4) { out.has_five_h_reset = true; out.five_h_reset_s = rdU32(v); }
                break;
            case kV7TagSevenDUsedPct:
                if (l >= 1) { out.has_seven_d_used = true; out.seven_d_used = v[0]; }
                break;
            case kV7TagSevenDResetInS:
                if (l >= 4) { out.has_seven_d_reset = true; out.seven_d_reset_s = rdU32(v); }
                break;
            case kV7TagContextUsedPct:
                if (l >= 1) { out.has_ctx = true; out.ctx_pct = v[0]; }
                break;
            case kV7TagCostMicroUsd:
                if (l >= 4) { out.has_cost = true; out.cost_micro_usd = rdU32(v); }
                break;
            case kV7TagDurationS:
                if (l >= 4) { out.has_duration = true; out.duration_s = rdU32(v); }
                break;
            case kV7TagLinesAdded:
                if (l >= 4) { out.has_lines_added = true; out.lines_added = rdU32(v); }
                break;
            case kV7TagLinesRemoved:
                if (l >= 4) { out.has_lines_removed = true; out.lines_removed = rdU32(v); }
                break;
            case kV7TagPlanMode:
                if (l >= 1) { out.has_mode = true; out.mode = v[0]; }
                break;
            case kV7TagTitle:
                out.has_title = true;
                cpyStr(out.title, sizeof(out.title), v, l);
                break;
            case kV7TagAggState:
                if (l >= 1) { out.has_agg_state = true; out.agg_state = v[0]; }
                break;
            case kV7TagActiveToolName:
                out.has_tool = true;
                cpyStr(out.tool, sizeof(out.tool), v, l);
                break;
            case kV7TagTotalSessions:
                if (l >= 1) { out.has_total = true; out.total_sessions = v[0]; }
                break;
            case kV7TagBusySessions:
                if (l >= 1) { out.has_busy = true; out.busy_sessions = v[0]; }
                break;
            case kV7TagInterventionKind:
                if (l >= 1) { out.has_intervention = true; out.intervention_kind = v[0]; }
                break;
            case kV7TagSessionEntry:
                // entry: [idx][client_id][state][kind][title_len][title…]
                if (l >= 5 && out.session_count < kMaxV7Sessions) {
                    V7Session& s = out.sessions[out.session_count++];
                    s.idx = v[0]; s.client_id = v[1]; s.state = v[2]; s.kind = v[3];
                    uint8_t tl = v[4];
                    uint8_t avail = static_cast<uint8_t>(l - 5);
                    cpyStr(s.title, sizeof(s.title), v + 5, tl < avail ? tl : avail);
                }
                break;
                // 未知 tag:跳过 value 区域,保证前向兼容
                break;
        }
        i += 2 + l;
    }
    return V7_OK;
}

}  // namespace cc_hud
