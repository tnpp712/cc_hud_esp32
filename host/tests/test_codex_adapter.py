from cchud_daemon.adapters.codex import CodexAdapter


def test_pretooluse_maps_to_tool():
    a = CodexAdapter()
    evs = a.normalize({"event": "PreToolUse",
                       "payload": {"session_id": "c1", "tool_name": "bash"}})
    e = [x for x in evs if x.kind == "state"][0]
    assert e.state == "tool" and e.detail == "bash"
    assert e.client_id == 1 and e.session_id == "c1"


def test_userpromptsubmit_and_stop():
    a = CodexAdapter()
    assert a.normalize({"event": "UserPromptSubmit",
                        "payload": {"session_id": "c1"}})[0].state == "thinking"
    assert a.normalize({"event": "Stop",
                        "payload": {"session_id": "c1"}})[0].state == "idle"


def test_request_user_input_tool_is_waiting():
    # 实测:Codex 用 request_user_input 工具问用户,应映射为 waiting 而非 tool
    a = CodexAdapter()
    evs = a.normalize({"event": "PreToolUse",
                       "payload": {"session_id": "c1",
                                   "tool_name": "request_user_input"}})
    assert [x for x in evs if x.kind == "state"][0].state == "waiting"


def test_permission_request_bypass_not_waiting():
    # 替我审批/bypass 模式:Codex 自动批准,不应亮红灯(非 waiting)
    a = CodexAdapter()
    evs = a.normalize({"event": "PermissionRequest",
                       "payload": {"session_id": "c1", "tool_name": "bash",
                                   "permission_mode": "bypassPermissions"}})
    assert [x for x in evs if x.kind == "state"][0].state != "waiting"


def test_permission_request_is_waiting():
    a = CodexAdapter()
    evs = a.normalize({"event": "PermissionRequest",
                       "payload": {"session_id": "c1", "tool_name": "bash"}})
    e = [x for x in evs if x.kind == "state"][0]
    assert e.state == "waiting" and e.detail == "bash"


def test_unknown_event_yields_nothing():
    assert CodexAdapter().normalize({"event": "SessionStart",
                                     "payload": {"session_id": "c1"}}) == []


def test_hook_spec_has_five_events():
    spec = CodexAdapter().hook_spec()
    assert set(spec.events) == {
        "UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop",
        "PermissionRequest"}
    assert spec.events["PreToolUse"] == "codex PreToolUse"
    assert spec.statusline_wrapper is None       # Codex 无 statusline 额度
