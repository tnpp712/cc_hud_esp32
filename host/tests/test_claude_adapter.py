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
