from cchud_daemon.session_store import SessionStore


def test_busy_session_counts_and_states():
    s = SessionStore(ttl_s=300)
    s.update("a", "thinking", "", now=1000)
    s.update("b", "tool", "Bash", now=1000)
    s.update("c", "idle", "", now=1000)
    assert s.counts(now=1000) == (3, 2)         # total=3, busy=2(idle 不计)
    assert ("tool", "Bash") in s.live_states(now=1000)


def test_expired_sessions_are_swept():
    s = SessionStore(ttl_s=300)
    s.update("old", "tool", "Bash", now=1000)
    s.update("new", "thinking", "", now=1400)
    # old 已超 300s(now=1400),应被剔除
    assert s.counts(now=1400) == (1, 1)
    assert s.live_states(now=1400) == [("thinking", "")]
