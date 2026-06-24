from cchud_daemon.session_store import SessionStore


def test_busy_session_counts_and_states():
    s = SessionStore(ttl_s=300)
    s.update("a", "thinking", "", now=1000)
    s.update("b", "tool", "Bash", now=1000)
    s.update("c", "idle", "", now=1000)
    assert s.counts(now=1000) == (3, 2)         # total=3, busy=2(idle 不计)
    assert ("tool", "Bash", "none") in s.live_states(now=1000)


def test_intervention_kind_stored():
    s = SessionStore()
    s.update("q", "waiting", "request_user_input", now=1000, kind="question")
    assert ("waiting", "request_user_input", "question") in s.live_states(now=1000)


def test_session_list_sorted_by_priority():
    s = SessionStore()
    s.update("a", "thinking", "", now=1000, client_id=0)
    s.update("b", "waiting", "ask", now=1000, kind="question", client_id=1)
    s.update("c", "tool", "Bash", now=1000, client_id=0)
    rows = s.session_list(now=1000)
    assert len(rows) == 3
    # waiting(优先级最高)排最前,带 client_id
    assert rows[0][0] == 1 and rows[0][1] == "waiting" and rows[0][3] == "question"
    # 末位是 thinking(优先级最低)
    assert rows[-1][1] == "thinking"


def test_expired_sessions_are_swept():
    s = SessionStore(ttl_s=300)
    s.update("old", "tool", "Bash", now=1000)
    s.update("new", "thinking", "", now=1400)
    # old 已超 300s(now=1400),应被剔除
    assert s.counts(now=1400) == (1, 1)
    assert s.live_states(now=1400) == [("thinking", "", "none")]
