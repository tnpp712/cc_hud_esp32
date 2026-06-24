from __future__ import annotations
import struct

# ── v7 tag 常量(对照路线图 7.4)────────────────────────────────────────
TAG_FIVE_H_USED_PCT      = 0x01
TAG_FIVE_H_RESET_IN_S    = 0x02
TAG_SEVEN_D_USED_PCT     = 0x03
TAG_SEVEN_D_RESET_IN_S   = 0x04
TAG_CONTEXT_USED_PCT     = 0x05
TAG_COST_MICRO_USD       = 0x06
TAG_DURATION_S           = 0x07
TAG_LINES_ADDED          = 0x08
TAG_LINES_REMOVED        = 0x09
TAG_PLAN_MODE            = 0x0A
TAG_TITLE                = 0x0B
TAG_AGG_STATE            = 0x20
TAG_ACTIVE_TOOL_NAME     = 0x21
TAG_TOTAL_SESSIONS       = 0x22
TAG_BUSY_SESSIONS        = 0x23
# 介入域(0x40-0x5F,第 3 层)
TAG_INTERVENTION_KIND    = 0x40
TAG_INTERVENTION_TOOL    = 0x41
TAG_INTERVENTION_TEXT    = 0x42
TAG_ALERT_CHANNELS       = 0x45
# 会话列表域(0x60-0x7F,第 4 层)
TAG_SESSION_LIST_COUNT   = 0x60
TAG_SESSION_ENTRY        = 0x61
TAG_UNIX_TS              = 0x80
TAG_UTC_OFFSET_MIN       = 0x81
TAG_STATUS_STRING        = 0x84

MSG_V7_TLV = 0x0B

# 状态码(与固件一致)
STATE_CODE = {"idle": 0, "thinking": 1, "tool": 2, "waiting": 3, "done": 4}
# 介入类型码
KIND_CODE = {"none": 0, "approval": 1, "question": 2, "error": 3}


def encode_quota_v6(*, mode, h5_used, h5_limit, d7_used, d7_limit,
                    h5_reset_s, d7_reset_s, cost_micro_usd, duration_s,
                    ctx_pct, lines_added, lines_removed, title) -> bytes:
    """v6 编码 (0x0A, 36 + title_len 字节)，与 push_quota.py:_pack_payload_v6 字节级一致"""
    title_b = title.encode("ascii", errors="replace")[:32]
    fixed = struct.pack(
        "<BBHHHHIIIIBIIB",
        0x0A, mode & 0xFF,
        h5_used, h5_limit, d7_used, d7_limit,
        h5_reset_s, d7_reset_s,
        cost_micro_usd, duration_s,
        max(0, min(100, ctx_pct)),
        max(0, lines_added), max(0, lines_removed),
        len(title_b),
    )
    return fixed + title_b


def encode_time_v4(unix_ts: int, utc_offset_min: int, status: str = "") -> bytes:
    """v4 时间帧(0x04),与 push_idle.py:_pack_v4 字节级一致。
    设备据此校准 RTC + 空闲页显示时钟与天气状态串。
    status 用 UTF-8(中文天气),按字节安全截断到 32 字节(不切断 CJK 字符)。"""
    s = status or ""
    b = s.encode("utf-8", errors="replace")
    while len(b) > 32:
        s = s[:-1]
        b = s.encode("utf-8", errors="replace")
    return (struct.pack("<BIhB", 0x04, unix_ts & 0xFFFFFFFF,
                        utc_offset_min, len(b)) + b)


def encode_state_0x07(state_code: int, detail: str, total: int, busy: int) -> bytes:
    """0x07 状态编码，与 push_state.py:pack_state 字节级一致"""
    d = detail.encode("ascii", errors="replace")[:15]
    base = struct.pack("<BBB", 0x07, state_code, len(d)) + d
    base += struct.pack("<BB", min(255, max(0, total)), min(255, max(0, busy)))
    return base


def encode_v7_state(state_code: int, detail: str, total: int, busy: int,
                    kind: str = "none", sessions=None) -> bytes:
    """v7 状态帧:AGG_STATE + 工具名 + 会话计数 + 介入类型(第 3 层)。

    取代旧 0x07,让状态/介入信息也走统一 TLV 帧。kind 为介入类型字符串。
    """
    fields = [
        tlv_u8(TAG_AGG_STATE, state_code),
        tlv_u8(TAG_TOTAL_SESSIONS, min(255, max(0, total))),
        tlv_u8(TAG_BUSY_SESSIONS, min(255, max(0, busy))),
    ]
    if detail:
        fields.append(tlv_str(TAG_ACTIVE_TOOL_NAME, detail))
    kind_code = KIND_CODE.get(kind or "none", 0)
    if kind_code != 0:
        fields.append(tlv_u8(TAG_INTERVENTION_KIND, kind_code))
        if detail:
            fields.append(tlv_str(TAG_INTERVENTION_TOOL, detail))
    # 第 4 层:会话列表(sessions 为 (client_id, state_code, kind_code, title) 列表)
    if sessions:
        fields.append(tlv_u8(TAG_SESSION_LIST_COUNT, min(255, len(sessions))))
        for idx, (cid, st_code, k_code, title) in enumerate(sessions):
            tb = (title or "").encode("utf-8", errors="replace")[:20]
            fields.append((TAG_SESSION_ENTRY,
                           bytes([idx & 0xFF, cid & 0xFF, st_code & 0xFF,
                                  k_code & 0xFF, len(tb)]) + tb))
    return encode_v7(fields)


def tlv_u8(tag: int, v: int) -> tuple[int, bytes]:
    """TLV 辅助: 8 位无符号整数"""
    return tag, struct.pack("<B", v & 0xFF)


def tlv_u16(tag: int, v: int) -> tuple[int, bytes]:
    """TLV 辅助: 16 位无符号整数"""
    return tag, struct.pack("<H", v & 0xFFFF)


def tlv_u32(tag: int, v: int) -> tuple[int, bytes]:
    """TLV 辅助: 32 位无符号整数"""
    return tag, struct.pack("<I", v & 0xFFFFFFFF)


def tlv_str(tag: int, s: str) -> tuple[int, bytes]:
    """TLV 辅助: UTF-8 字符串"""
    return tag, s.encode("utf-8", errors="replace")


def encode_v7(fields: list[tuple[int, bytes]]) -> bytes:
    """v7 TLV 组帧: 帧头 [0x0B, flags=0, seq=0, total=1] + 连续 [tag][len][value]"""
    out = bytearray([MSG_V7_TLV, 0, 0, 1])      # msg_type, flags, seq, total
    for tag, value in fields:
        if len(value) > 255:
            raise ValueError(f"tag 0x{tag:02X} value too long: {len(value)}")
        out += bytes([tag & 0xFF, len(value)]) + value
    return bytes(out)
