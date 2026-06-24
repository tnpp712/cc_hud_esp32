from cchud_daemon.aggregator import Aggregator
from cchud_daemon.session_store import SessionStore


def test_aggregate_picks_highest_priority():
    s = SessionStore()
    s.update("a", "thinking", "", now=1000)
    s.update("b", "tool", "Bash", now=1000)
    agg = Aggregator()
    assert agg.aggregate(s, now=1000) == ("tool", "Bash", "none", 2, 2)


def test_aggregate_empty_is_idle():
    assert Aggregator().aggregate(SessionStore(), now=1000) == ("idle", "", "none", 0, 0)


def test_aggregate_carries_intervention_kind():
    s = SessionStore()
    s.update("a", "thinking", "", now=1000)
    s.update("b", "waiting", "ask", now=1000, kind="question")
    # 最高优先级 waiting 会话的 kind 应被带出
    assert Aggregator().aggregate(s, now=1000) == ("waiting", "ask", "question", 2, 2)


def test_changed_dedup():
    agg = Aggregator()
    key = ("tool", "Bash", 2, 2)
    assert agg.changed(key) is True     # 首次
    assert agg.changed(key) is False    # 不变
    assert agg.changed(("idle", "", 0, 0)) is True
