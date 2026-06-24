from __future__ import annotations


class QuotaTracker:
    """额度跨会话分窗合并 + BLE 推送限频(复刻 cchud-quota-push.sh / cchud-update.sh)。"""

    def __init__(self, rate_limit_s: float = 30.0) -> None:
        self._rate = rate_limit_s
        self._last_push: float | None = None
        # window -> (used, resets_at)
        self._win: dict[str, tuple[int, int]] = {}

    def merge(self, used: int, resets_at: int, window: str, now: float) -> tuple[int, int]:
        cur = self._win.get(window)
        if cur is None or resets_at > cur[1]:
            merged = (used, resets_at)
        elif resets_at == cur[1]:
            merged = (max(used, cur[0]), resets_at)
        else:
            merged = cur                       # incoming 是旧窗口,忽略
        self._win[window] = merged
        return merged

    def should_push(self, now: float) -> bool:
        return self._last_push is None or (now - self._last_push) >= self._rate

    def mark_pushed(self, now: float) -> None:
        self._last_push = now
