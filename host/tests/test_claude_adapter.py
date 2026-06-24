from cchud_daemon.adapters.claude import ClaudeAdapter


def test_pretooluse_maps_to_tool():
    a = ClaudeAdapter()
    evs = a.normalize({"event": "PreToolUse",
                       "payload": {"session_id": "s1", "tool_name": "Bash"}})
    e = [x for x in evs if x.kind == "state"][0]
    assert e.state == "tool" and e.detail == "Bash" and e.session_id == "s1"


def test_notification_permission_is_waiting():
    a = ClaudeAdapter()
    evs = a.normalize({"event": "Notification",
                       "payload": {"session_id": "s1",
                                   "message": "Claude needs your permission to run Bash"}})
    assert [x for x in evs if x.kind == "state"][0].state == "waiting"


def test_notification_benign_downgrades_to_idle():
    a = ClaudeAdapter()
    evs = a.normalize({"event": "Notification",
                       "payload": {"session_id": "s1",
                                   "message": "Claude is waiting for your input"}})
    assert [x for x in evs if x.kind == "state"][0].state == "idle"


def test_statusline_produces_quota_event_with_title():
    a = ClaudeAdapter()
    # payload 本身即 statusLine 的 stdin JSON(emit 原样透传)
    payload = {"session_id": "s1",
        "rate_limits": {"five_hour": {"used_percentage": 40, "resets_at": 2000},
                        "seven_day": {"used_percentage": 28, "resets_at": 9000}},
        "context_window": {"used_percentage": 65, "context_window_size": 1000000},
        "cost": {"total_cost_usd": 0.42, "total_duration_ms": 23000,
                 "total_lines_added": 127, "total_lines_removed": 45},
        "model": {"display_name": "Opus 4.8"}}
    q = [x for x in a.normalize({"event": "Status", "payload": payload})
         if x.kind == "quota"][0]
    assert q.five_h_used == 40 and q.seven_d_resets_at == 9000
    assert q.ctx_pct == 65 and q.lines_added == 127
    assert q.title == "Opus 4.8 (1M context)"


def test_statusline_title_k_context():
    """ctx_size 在 1000~999999(如 200000)→ 标题含 'K context'。"""
    a = ClaudeAdapter()
    payload = {
        "session_id": "s1",
        "rate_limits": {"five_hour": {}, "seven_day": {}},
        "context_window": {"used_percentage": 50, "context_window_size": 200000},
        "cost": {},
        "model": {"display_name": "Sonnet 4.5"},
    }
    q = [x for x in a.normalize({"event": "Status", "payload": payload})
         if x.kind == "quota"][0]
    assert q.title == "Sonnet 4.5 (200K context)", f"实际 title={q.title!r}"


def test_statusline_title_no_context_size():
    """model 存在但无 context_window_size(或 size<1000)→ 标题为纯 model 名(无括号)。"""
    a = ClaudeAdapter()
    # 情形 A:context_window_size 为 0(小于 1000 阈值)
    payload_zero = {
        "session_id": "s1",
        "rate_limits": {},
        "context_window": {"used_percentage": 30, "context_window_size": 0},
        "cost": {},
        "model": {"display_name": "Haiku 3.5"},
    }
    q = [x for x in a.normalize({"event": "Status", "payload": payload_zero})
         if x.kind == "quota"][0]
    assert q.title == "Haiku 3.5", f"情形A 实际 title={q.title!r}"

    # 情形 B:context_window_size 缺失
    payload_missing = {
        "session_id": "s1",
        "rate_limits": {},
        "context_window": {"used_percentage": 10},
        "cost": {},
        "model": {"display_name": "Haiku 3.5"},
    }
    q2 = [x for x in a.normalize({"event": "Status", "payload": payload_missing})
          if x.kind == "quota"][0]
    assert q2.title == "Haiku 3.5", f"情形B 实际 title={q2.title!r}"


def test_statusline_title_no_model():
    """model 缺失 → quota 事件 title 为 None。"""
    a = ClaudeAdapter()
    payload = {
        "session_id": "s1",
        "rate_limits": {"five_hour": {"used_percentage": 10, "resets_at": 1000},
                        "seven_day": {"used_percentage": 5, "resets_at": 5000}},
        "context_window": {"used_percentage": 20, "context_window_size": 1000000},
        "cost": {"total_cost_usd": 0.01, "total_duration_ms": 5000,
                 "total_lines_added": 0, "total_lines_removed": 0},
        # model 字段完全缺失
    }
    q = [x for x in a.normalize({"event": "Status", "payload": payload})
         if x.kind == "quota"][0]
    assert q.title is None, f"实际 title={q.title!r},应为 None"
