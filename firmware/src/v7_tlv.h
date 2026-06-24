#pragma once
#include <cstdint>
#include <cstddef>

namespace cc_hud {

// v7 tag 常量(对照路线图 7.4,本层用到的子集)
enum V7Tag : uint8_t {
    kV7TagFiveHUsedPct    = 0x01,
    kV7TagFiveHResetInS   = 0x02,
    kV7TagSevenDUsedPct   = 0x03,
    kV7TagSevenDResetInS  = 0x04,
    kV7TagContextUsedPct  = 0x05,
    kV7TagCostMicroUsd    = 0x06,
    kV7TagDurationS       = 0x07,
    kV7TagLinesAdded      = 0x08,
    kV7TagLinesRemoved    = 0x09,
    kV7TagPlanMode        = 0x0A,
    kV7TagTitle           = 0x0B,
    kV7TagAggState        = 0x20,
    kV7TagActiveToolName  = 0x21,
    kV7TagTotalSessions   = 0x22,
    kV7TagBusySessions    = 0x23,
    kV7TagInterventionKind = 0x40,   // 0=none 1=approval 2=question 3=error
    kV7TagInterventionTool = 0x41,
};

enum V7Result { V7_OK, V7_ERR_LEN, V7_ERR_FRAGMENT };

struct V7Fields {
    bool     has_five_h_used    = false;  uint8_t  five_h_used     = 0;
    bool     has_five_h_reset   = false;  uint32_t five_h_reset_s  = 0;
    bool     has_seven_d_used   = false;  uint8_t  seven_d_used    = 0;
    bool     has_seven_d_reset  = false;  uint32_t seven_d_reset_s = 0;
    bool     has_ctx            = false;  uint8_t  ctx_pct         = 0;
    bool     has_cost           = false;  uint32_t cost_micro_usd  = 0;
    bool     has_duration       = false;  uint32_t duration_s      = 0;
    bool     has_lines_added    = false;  uint32_t lines_added     = 0;
    bool     has_lines_removed  = false;  uint32_t lines_removed   = 0;
    bool     has_mode           = false;  uint8_t  mode            = 0;
    bool     has_title          = false;  char     title[33]       = {0};
    bool     has_agg_state      = false;  uint8_t  agg_state       = 0;
    bool     has_tool           = false;  char     tool[16]        = {0};
    bool     has_total          = false;  uint8_t  total_sessions  = 0;
    bool     has_busy           = false;  uint8_t  busy_sessions   = 0;
    bool     has_intervention   = false;  uint8_t  intervention_kind = 0;
};

// 解析 v7 TLV 帧:校验帧头(buf[0]==0x0B, total==1),循环读 [tag][len][value]。
// 越界返回 V7_ERR_LEN,分片(total!=1)返回 V7_ERR_FRAGMENT,未知 tag 跳过。
V7Result parseV7Tlv(const uint8_t* buf, size_t len, V7Fields& out);

}  // namespace cc_hud
