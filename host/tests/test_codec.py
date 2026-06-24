import struct
from cchud_daemon import codec


def _parse_tlv(frame):
    tags = {}
    i = 4
    while i + 2 <= len(frame):
        t, l = frame[i], frame[i + 1]
        tags[t] = frame[i + 2:i + 2 + l]
        i += 2 + l
    return tags


def test_v7_state_frame_with_intervention():
    frame = codec.encode_v7_state(3, "ask", total=1, busy=1, kind="question")
    assert frame[:4] == bytes([0x0B, 0, 0, 1])
    tags = _parse_tlv(frame)
    assert tags[codec.TAG_AGG_STATE] == bytes([3])          # waiting
    assert tags[codec.TAG_INTERVENTION_KIND] == bytes([2])  # question
    assert tags[codec.TAG_INTERVENTION_TOOL] == b"ask"


def test_v7_state_no_intervention_tag_when_none():
    frame = codec.encode_v7_state(2, "Bash", total=1, busy=1, kind="none")
    tags = _parse_tlv(frame)
    assert codec.TAG_INTERVENTION_KIND not in tags          # 非介入不发 kind
    assert tags[codec.TAG_ACTIVE_TOOL_NAME] == b"Bash"
    assert tags[codec.TAG_AGG_STATE] == bytes([2])          # tool


def test_v6_matches_legacy_layout():
    # 与 push_quota.py 的 _pack_payload_v6 字节级一致
    p = codec.encode_quota_v6(
        mode=0, h5_used=40, h5_limit=100, d7_used=28, d7_limit=100,
        h5_reset_s=120, d7_reset_s=3600, cost_micro_usd=420000,
        duration_s=23, ctx_pct=65, lines_added=127, lines_removed=45,
        title="Opus 4.8 (1M context)")
    assert p[0] == 0x0A                       # v6 msg_type
    title_b = "Opus 4.8 (1M context)".encode("ascii")[:32]
    assert p[-len(title_b):] == title_b
    assert p[35] == len(title_b)              # title_len 偏移


def test_state_0x07_layout():
    p = codec.encode_state_0x07(2, "Bash", total=3, busy=2)
    assert p[0] == 0x07 and p[1] == 2 and p[2] == 4
    assert p[3:7] == b"Bash"
    assert p[7] == 3 and p[8] == 2            # total, busy 尾字节


def test_v7_frame_and_tlv():
    frame = codec.encode_v7([
        codec.tlv_u8(codec.TAG_FIVE_H_USED_PCT, 40),
        codec.tlv_str(codec.TAG_TITLE, "Opus"),
    ])
    assert frame[:4] == bytes([0x0B, 0, 0, 1])         # 头
    # 第一个字段:tag=0x01 len=1 value=40
    assert frame[4] == 0x01 and frame[5] == 1 and frame[6] == 40
    # 第二个字段:tag=TAG_TITLE len=4 "Opus"
    off = 7
    assert frame[off] == codec.TAG_TITLE and frame[off + 1] == 4
    assert frame[off + 2:off + 6] == b"Opus"


def test_v7_rejects_oversize_field():
    import pytest
    with pytest.raises(ValueError):
        codec.encode_v7([codec.tlv_str(codec.TAG_TITLE, "x" * 300)])
