from cchud_daemon.quota_tracker import QuotaTracker


def test_merge_takes_max_in_same_window():
    q = QuotaTracker()
    assert q.merge(40, 2000, "5h", now=1000) == (40, 2000)
    # 同窗口(resets_at 相同),取较大 used
    assert q.merge(35, 2000, "5h", now=1001) == (40, 2000)
    assert q.merge(55, 2000, "5h", now=1002) == (55, 2000)


def test_merge_replaces_on_newer_window():
    q = QuotaTracker()
    q.merge(90, 2000, "5h", now=1000)
    # 新窗口(resets_at 更大)→ 直接替换,处理用量归零
    assert q.merge(5, 9000, "5h", now=2001) == (5, 9000)


def test_rate_limit():
    q = QuotaTracker(rate_limit_s=30)
    assert q.should_push(now=1000) is True
    q.mark_pushed(now=1000)
    assert q.should_push(now=1020) is False    # 不足 30s
    assert q.should_push(now=1031) is True      # 超过 30s
