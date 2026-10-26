"""只读增量采集 Codex 本地会话用量，不读取凭据或发送会话正文。"""
from __future__ import annotations

import json
import time
import sqlite3
from datetime import datetime, timedelta
from pathlib import Path

from .event import CcHudEvent
from .questions import QuestionTracker
from .session_identity import SessionIdentity


def changed_lines(changes: dict) -> tuple[int, int] | None:
    """统计 Codex 确认成功的文件变更；未知格式保持未知，不猜测 shell 写入。"""
    added = removed = 0
    for change in changes.values():
        kind = change.get("type")
        if kind == "add":
            added += len(change.get("content", "").splitlines())
        elif kind == "delete" and "content" in change:
            removed += len(change["content"].splitlines())
        elif kind == "update" and "unified_diff" in change:
            for line in change["unified_diff"].splitlines():
                if line.startswith("+") and not line.startswith("+++"):
                    added += 1
                elif line.startswith("-") and not line.startswith("---"):
                    removed += 1
        else:
            return None
    return added, removed


def usage_event(payload: dict, session_id: str, duration_s: int | None = None) -> CcHudEvent:
    """只采集 7 天额度；primary / secondary 的位置不代表窗口时长。"""
    event = CcHudEvent(1, session_id, "quota", title="Codex", mode="sub",
                       duration_s=duration_s, metrics_valid=0)
    limits = payload.get("rate_limits") or {}
    if limits.get("limit_id") in (None, "codex"):
        for name in ("primary", "secondary"):
            window = limits.get(name) or {}
            minutes, used = window.get("window_minutes"), window.get("used_percent")
            if used is None or minutes != 10080:
                continue
            pct = max(0, min(100, round(used)))
            event.seven_d_used = pct
            event.seven_d_resets_at = window.get("resets_at")
            event.metrics_valid |= 2
    info = payload.get("info") or {}
    context = info.get("model_context_window")
    usage = info.get("last_token_usage") or {}
    tokens = usage.get("total_tokens")
    if context and tokens is not None:
        event.ctx_pct = max(0, min(100, round(tokens * 100 / context)))
        event.metrics_valid |= 4
    if duration_s is not None:
        event.metrics_valid |= 16
    return event


class CodexUsageReader:
    """通过本地索引与 hook 查找活跃会话，增量读取日志和会话名称。"""

    def __init__(self, home: Path, now=time.time, closure_path: Path | None = None, sources_path: Path | None = None):
        self.home, self.now = home, now
        self.questions = QuestionTracker(closure_path)
        self.identity = SessionIdentity(sources_path.with_suffix(".identities.json") if sources_path else None)
        self.titles = {}
        self.tracked = set()
        self.sources_path = sources_path
        if sources_path:
            try:
                for value in json.loads(sources_path.read_text()):
                    candidate=Path(value).resolve()
                    if candidate.is_relative_to((home/"sessions").resolve()): self.tracked.add(candidate)
            except (OSError, ValueError, TypeError): pass
        self.discovered = set()
        self.discovery_at = 0
        self.offsets: dict[Path, int] = {}
        self.started: dict[Path, float] = {}
        self.ids: dict[Path, str] = {}
        self.metadata = {}
        self.ignored = set()
        self.lines: dict[Path, tuple[int, int] | None] = {}
        self.changes_seen: set[tuple[Path, str]] = set()

    def track_path(self, path):
        if not isinstance(path, str): return
        candidate = Path(path).resolve()
        if candidate.is_relative_to((self.home / "sessions").resolve()) and candidate not in self.tracked:
            self.tracked.add(candidate)
            if len(self.tracked)>64:
                self.tracked=set(sorted(self.tracked,key=lambda p:p.stat().st_mtime if p.exists() else 0)[-64:])
            self._save_sources()

    def untrack_path(self, path):
        if isinstance(path,str):
            self.tracked.discard(Path(path).resolve())
            self._save_sources()

    def _save_sources(self):
        if self.sources_path:
            self.sources_path.parent.mkdir(parents=True,exist_ok=True)
            temp=self.sources_path.with_suffix('.tmp')
            temp.write_text(json.dumps([str(p) for p in self.tracked]))
            temp.replace(self.sources_path)

    def _files(self, now):
        if now - self.discovery_at >= 30:
            files = set()
            database = self.home / "state_5.sqlite"
            if database.exists():
                try:
                    with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as connection:
                        rows = connection.execute("SELECT rollout_path FROM threads WHERE archived=0 AND updated_at>=? ORDER BY updated_at DESC LIMIT 64", (int(now)-3600,)).fetchall()
                        columns = {r[1] for r in connection.execute('PRAGMA table_info(threads)')}
                        if 'title' in columns:
                            paths = {r[0] for r in rows} | {str(p) for p in self.tracked}
                            if paths:
                                placeholders = ','.join('?' for _ in paths)
                                names = connection.execute(f'SELECT rollout_path,title FROM threads WHERE rollout_path IN ({placeholders})', tuple(paths)).fetchall()
                                self.titles = {Path(p).resolve(): ' '.join((title or '').split()) for p,title in names}
                    for (value,) in rows:
                        path=Path(value).resolve()
                        if path.is_relative_to((self.home/"sessions").resolve()): files.add(path)
                except (sqlite3.Error, OSError): pass
            # 独立 CLI 或测试环境可能没有会话索引，保留短范围目录发现作为降级。
            if not database.exists():
                for delta in (0,1):
                    day=datetime.fromtimestamp(now)-timedelta(days=delta)
                    files.update((self.home/"sessions"/day.strftime("%Y/%m/%d")).glob("*.jsonl"))
            self.discovered=files
            self.discovery_at=now
        files=set(self.tracked)|self.discovered
        present=[]
        for path in files:
            try: present.append((path.stat().st_mtime,path))
            except OSError: pass
        return [p for _,p in sorted(present)]

    def poll(self) -> list[CcHudEvent]:
        now = self.now()
        files = self._files(now)
        updates = {}
        latest = None
        latest_ts = -1.0
        for path in files:
            if path in self.ignored:
                continue
            stat = path.stat()
            if now - stat.st_mtime > 3600 and path not in self.tracked:
                continue
            offset = self.offsets.get(path, 0)
            if stat.st_size < offset:
                offset = 0
                self.questions.reset(self.ids.get(path,path.stem))
                self.metadata.pop(path,None)
                self.started.pop(path, None)
                self.lines.pop(path, None)
                self.changes_seen = {key for key in self.changes_seen if key[0] != path}
            read_any = False
            with path.open("rb") as source:
                source.seek(offset)
                while True:
                    line = source.readline()
                    if not line or not line.endswith(b"\n"):
                        break
                    self.offsets[path] = source.tell()
                    read_any = True
                    try:
                        row = json.loads(line)
                        payload = row.get("payload") or {}
                        ts = datetime.fromisoformat(row["timestamp"].replace("Z", "+00:00")).timestamp()
                    except (ValueError, KeyError, TypeError):
                        continue
                    if row.get("type") == "session_meta":
                        self.ids[path] = payload.get("id", path.stem)
                    sid = self.ids.get(path, path.stem)
                    self.questions.ingest(sid,row,ts)
                    if row.get("type") == "turn_context":
                        if payload.get("model") == "codex-auto-review":
                            self.ignored.add(path)
                            updates.pop(sid, None)
                            self.questions.reset(sid)
                            break
                        meta = CcHudEvent(1, sid, "metadata", turn_id=payload.get("turn_id"),
                            project=Path(payload.get("cwd", "")).name or None,
                            model=payload.get("model"), effort=payload.get("effort"), ts=ts)
                        meta.started_at = self.started.get(path)
                        self.metadata[path] = meta
                        updates[sid] = meta
                    if row.get("type") != "event_msg":
                        continue
                    if payload.get("type") == "task_started":
                        self.started[path] = ts
                        meta = self.metadata.get(path)
                        if meta and (not meta.turn_id or meta.turn_id == payload.get("turn_id")):
                            meta.started_at = ts
                            meta.ended_at = None
                            updates[sid] = meta
                    if payload.get("type") in ("task_complete", "turn_aborted"):
                        meta = self.metadata.get(path)
                        if meta and meta.turn_id == payload.get("turn_id"):
                            meta.ended_at = ts
                            updates[sid] = meta
                    item = payload.get("item") or {}
                    key = (path, item.get("id", ""))
                    if (payload.get("type") == "item_completed" and
                            item.get("type") == "FileChange" and item.get("status") == "completed"
                            and key not in self.changes_seen):
                        self.changes_seen.add(key)
                        counts = changed_lines(item.get("changes") or {})
                        previous = self.lines.get(path, (0, 0))
                        self.lines[path] = (tuple(a+b for a,b in zip(previous, counts))
                                            if previous is not None and counts is not None else None)
                    if payload.get("type") == "token_count":
                        per_session = usage_event(payload, sid)
                        meta = self.metadata.get(path)
                        if meta and per_session.ctx_pct is not None:
                            meta.ctx_pct = per_session.ctx_pct
                            updates[sid] = meta
                    if payload.get("type") == "token_count" and now - ts <= 300 and ts >= latest_ts:
                        duration = max(0, int(ts - self.started[path])) if path in self.started else None
                        latest = usage_event(payload, self.ids.get(path, path.stem), duration)
                        if self.lines.get(path) is not None:
                            latest.lines_added, latest.lines_removed = self.lines[path]
                            latest.metrics_valid |= 32
                        latest_ts = ts
            meta = self.metadata.get(path)
            if meta and path not in self.ignored:
                title = self.titles.get(path, '')
                code = self.identity.code(meta.session_id)
                identity_changed = (meta.session_title, meta.short_id) != (title, code)
                meta.session_title, meta.short_id = title, code
                if not read_any and not identity_changed:
                    continue
                meta.pending_questions = self.questions.count(meta.session_id)
                meta.question_ids = self.questions.ids(meta.session_id)
                updates[meta.session_id] = meta
        return list(updates.values()) + ([latest] if latest else [])
