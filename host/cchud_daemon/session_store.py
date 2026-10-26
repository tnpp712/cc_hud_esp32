from __future__ import annotations
import zlib

# 状态优先级:waiting > tool > thinking > idle(复刻 cchud-hook.sh)
_PRIORITY = {"waiting": 3, "tool": 2, "thinking": 1, "done": 0, "idle": 0}


class SessionStore:
    """每会话最新状态 + 最后活动时刻;过期会话视为已结束(复刻 TTL 扫描)。"""

    def __init__(self, ttl_s: float = 300.0) -> None:
        self._ttl = ttl_s
        # sid -> (state, detail, intervention_kind, client_id, last_seen)
        self._sessions: dict[str, tuple[str, str, str, int, float]] = {}
        self._meta = {}
        self._pending = {}
        self._closed = set()
        self._history = {}

    def pending_ids(self, sid):
        return list(self._pending.get(sid,{}).get("ids",[]))

    def apply(self, event, now):
        sid = event.session_id
        hydrated = False
        if event.session_end:
            self.work_rows(now)
            self._sessions.pop(sid, None)
            self._meta.pop(sid, None)
            self._pending.pop(sid, None)
            self._closed.add(sid)
            return
        if event.kind == "state":
            self._closed.discard(sid)
        if event.pending_questions is not None:
            self._pending[sid] = {"count":max(0,event.pending_questions),"ids":event.question_ids or []}
            if event.pending_questions and sid not in self._sessions and sid not in self._closed:
                self.update(sid,"idle","",event.ts or now,client_id=event.client_id)
                hydrated = True
        meta = self._meta.setdefault(sid, {})
        if event.turn_id and meta.get("turn_id") != event.turn_id:
            if event.kind == "metadata" and event.ts < meta.get("updated_at", 0):
                return
            self.work_rows(now)
            identity = {key: meta.get(key) for key in ("project", "project_name", "model", "session_title", "short_id")}
            meta.clear()
            meta.update(identity, turn_id=event.turn_id)
        if hydrated:
            meta["phase"] = "idle" if event.ended_at is not None else "unknown"
        for key in ("project", "project_name", "session_title", "short_id", "model", "effort", "phase", "plan", "started_at", "ended_at", "ctx_pct", "progress", "attention", "result", "outcome"):
            if key=='attention' and event.kind=='metadata' and not event.pending_questions and self._sessions.get(sid,('', '', '',0,0))[2]=='approval':
                continue
            value = getattr(event, key)
            if value is not None:
                meta[key] = value
        meta["updated_at"] = max(meta.get("updated_at", 0), event.ts or now)
        if event.kind == "state":
            if event.started_at is not None:
                meta.pop("ended_at", None)
                meta["plan"] = []
            self.update(sid, event.state or "idle", event.detail or "", now,
                        kind=event.intervention_kind or "none", client_id=event.client_id)
        if event.outcome==3 and event.ended_at and now-event.ended_at<=300:
            meta['phase']='error'
            self.update(sid,'waiting','执行失败',now,kind='error',client_id=event.client_id)
        if event.ended_at is not None:
            self.work_rows(now)

    def work_rows(self, now):
        self._sweep(now)
        rows = []
        current_results = set()
        for sid, (state, detail, kind, cid, ts) in self._sessions.items():
            meta = self._meta.get(sid, {})
            plan = meta.get("plan") or []
            valid = bool(plan) and all(p.get("status") in ("pending", "in_progress", "completed") for p in plan)
            active = next((p.get("step", "") for p in plan if p.get("status") == "in_progress"), "") if valid else ""
            started = meta.get("started_at")
            elapsed = max(0, int((meta.get("ended_at") or now) - started)) if started else 0
            rows.append(dict(key=zlib.crc32(sid.encode()), state=state, detail=detail,
                             kind=kind, client_id=cid, project=meta.get("project_name") or meta.get("project") or "",
                             session_title=meta.get("session_title") or "", short_id=meta.get("short_id") or "",
                             model=meta.get("model") or "", effort=meta.get("effort") or "",
                             phase=meta.get("phase") or state, elapsed=elapsed,
                             time_known=started is not None, plan_known=valid,
                             plan_done=sum(p.get("status") == "completed" for p in plan) if valid else 0,
                             plan_total=len(plan) if valid else 0, step=active, last_seen=ts))
            rows[-1]["pending_questions"] = self._pending.get(sid,{}).get("count",0)
            rows[-1]["question_ids"] = self._pending.get(sid,{}).get("ids",[])
            rows[-1]["ctx_pct"] = meta.get("ctx_pct")
            rows[-1].update(progress=meta.get('progress',''),attention=meta.get('attention',''),
                            result=meta.get('result',''),outcome=meta.get('outcome',0),
                            ended_at=int(meta.get('ended_at') or 0),history=False,
                            result_key=zlib.crc32(f'{sid}:{meta.get("turn_id")}'.encode()))
            if rows[-1]['outcome'] and rows[-1]['ended_at'] and now-rows[-1]['ended_at']<=1800:
                ident=(sid,meta.get('turn_id'))
                current_results.add(ident)
                saved=dict(rows[-1],key=zlib.crc32(f'{sid}:{meta.get("turn_id")}:result'.encode()),
                           state='idle',kind='none',phase='idle',history=True,pending_questions=0,attention='')
                self._history[ident]=saved
        rows.sort(key=lambda r: (3 if r["pending_questions"] else _PRIORITY.get(r["state"], 0), r["last_seen"]), reverse=True)
        self._history=dict(sorted(((k,v) for k,v in self._history.items() if now-v['ended_at']<=1800),
                                  key=lambda item:item[1]['ended_at'],reverse=True)[:3])
        return rows + [dict(row) for ident,row in self._history.items() if ident not in current_results]

    def update(self, session_id: str, state: str, detail: str, now: float,
               kind: str = "none", client_id: int = 0) -> None:
        self._sessions[session_id] = (state, detail, kind, client_id, now)

    def _sweep(self, now: float) -> None:
        dead = [sid for sid, (_, _, _, _, ts) in self._sessions.items()
                if now - ts > self._ttl and not self._pending.get(sid,{}).get("count",0)]
        for sid in dead:
            del self._sessions[sid]

    def live_states(self, now: float) -> list[tuple[str, str, str]]:
        self._sweep(now)
        return [("waiting", "pending-input", "question") if self._pending.get(sid,{}).get("count",0)
                else (st, det, kind)
                for sid, (st, det, kind, _, _) in self._sessions.items()]

    def counts(self, now: float) -> tuple[int, int]:
        self._sweep(now)
        total = len(self._sessions)
        busy = sum(1 for sid,(st, _, _, _, _) in self._sessions.items()
                   if _PRIORITY.get(st, 0) > 0 or self._pending.get(sid,{}).get("count",0))
        return total, busy

    def session_list(self, now: float) -> list[tuple[int, str, str, str]]:
        """返回按优先级降序排序的会话列表 (client_id, state, detail, kind);
        优先级高者(waiting>tool>thinking>idle)在前,即"等你的"排最前。"""
        self._sweep(now)
        rows = [(cid, "waiting", "pending-input", "question") if self._pending.get(sid,{}).get("count",0)
                else (cid, st, det, kind)
                for sid, (st, det, kind, cid, _) in self._sessions.items()]
        rows.sort(key=lambda r: _PRIORITY.get(r[1], 0), reverse=True)
        return rows
