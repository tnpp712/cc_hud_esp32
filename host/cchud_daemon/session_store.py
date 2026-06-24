from __future__ import annotations

# 状态优先级:waiting > tool > thinking > idle(复刻 cchud-hook.sh)
_PRIORITY = {"waiting": 3, "tool": 2, "thinking": 1, "idle": 0}


class SessionStore:
    """每会话最新状态 + 最后活动时刻;过期会话视为已结束(复刻 TTL 扫描)。"""

    def __init__(self, ttl_s: float = 300.0) -> None:
        self._ttl = ttl_s
        self._sessions: dict[str, tuple[str, str, float]] = {}

    def update(self, session_id: str, state: str, detail: str, now: float) -> None:
        self._sessions[session_id] = (state, detail, now)

    def _sweep(self, now: float) -> None:
        dead = [sid for sid, (_, _, ts) in self._sessions.items()
                if now - ts > self._ttl]
        for sid in dead:
            del self._sessions[sid]

    def live_states(self, now: float) -> list[tuple[str, str]]:
        self._sweep(now)
        return [(st, det) for (st, det, _) in self._sessions.values()]

    def counts(self, now: float) -> tuple[int, int]:
        self._sweep(now)
        total = len(self._sessions)
        busy = sum(1 for (st, _, _) in self._sessions.values()
                   if _PRIORITY.get(st, 0) > 0)
        return total, busy
