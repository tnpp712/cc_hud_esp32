from __future__ import annotations

from .session_store import SessionStore, _PRIORITY


class Aggregator:
    """把多会话聚合为单一推送态,并对不变结果去重(复刻 cchud-hook.sh)。"""

    def __init__(self) -> None:
        self._last_key: tuple | None = None

    def aggregate(self, store: SessionStore, now: float) -> tuple[str, str, int, int]:
        best_state, best_detail, best_p = "idle", "", -1
        for state, detail in store.live_states(now):
            p = _PRIORITY.get(state, 0)
            if p > best_p:
                best_p, best_state, best_detail = p, state, detail
        total, busy = store.counts(now)
        return best_state, best_detail, total, busy

    def changed(self, key: tuple) -> bool:
        if key == self._last_key:
            return False
        self._last_key = key
        return True
