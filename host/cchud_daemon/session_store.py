from __future__ import annotations

# 状态优先级:waiting > tool > thinking > idle(复刻 cchud-hook.sh)
_PRIORITY = {"waiting": 3, "tool": 2, "thinking": 1, "idle": 0}


class SessionStore:
    """每会话最新状态 + 最后活动时刻;过期会话视为已结束(复刻 TTL 扫描)。"""

    def __init__(self, ttl_s: float = 300.0) -> None:
        self._ttl = ttl_s
        # sid -> (state, detail, intervention_kind, client_id, last_seen)
        self._sessions: dict[str, tuple[str, str, str, int, float]] = {}

    def update(self, session_id: str, state: str, detail: str, now: float,
               kind: str = "none", client_id: int = 0) -> None:
        self._sessions[session_id] = (state, detail, kind, client_id, now)

    def _sweep(self, now: float) -> None:
        dead = [sid for sid, (_, _, _, _, ts) in self._sessions.items()
                if now - ts > self._ttl]
        for sid in dead:
            del self._sessions[sid]

    def live_states(self, now: float) -> list[tuple[str, str, str]]:
        self._sweep(now)
        return [(st, det, kind)
                for (st, det, kind, _, _) in self._sessions.values()]

    def counts(self, now: float) -> tuple[int, int]:
        self._sweep(now)
        total = len(self._sessions)
        busy = sum(1 for (st, _, _, _, _) in self._sessions.values()
                   if _PRIORITY.get(st, 0) > 0)
        return total, busy

    def session_list(self, now: float) -> list[tuple[int, str, str, str]]:
        """返回按优先级降序排序的会话列表 (client_id, state, detail, kind);
        优先级高者(waiting>tool>thinking>idle)在前,即"等你的"排最前。"""
        self._sweep(now)
        rows = [(cid, st, det, kind)
                for (st, det, kind, cid, _) in self._sessions.values()]
        rows.sort(key=lambda r: _PRIORITY.get(r[1], 0), reverse=True)
        return rows
