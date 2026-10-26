from __future__ import annotations
import logging
import time
from pathlib import Path
from ..event import CcHudEvent
from ..approval_context import read_approval_reviewer
from .base import HookSpec

_WAIT_TOOLS = {"request_user_input", "functions.request_user_input"}
_TOOL_NAMES = {"exec_command": "Bash", "functions.exec_command": "Bash",
               "apply_patch": "Edit", "functions.apply_patch": "Edit",
               "write_stdin": "Bash", "functions.write_stdin": "Bash",
               "web.run": "WebSearch"}


def tool_phase(name):
    if name in ("Bash", "bash", "shell"): return "command"
    if name in ("Edit", "Write", "apply_patch", "MultiEdit"): return "edit"
    if name in ("Read", "read_file"): return "read"
    if name in ("Grep", "Glob", "WebSearch", "WebFetch") or "search" in name.lower(): return "search"
    if name.startswith("mcp__"): return "mcp"
    if "plan" in name.lower(): return "plan"
    return "tool"


class CodexAdapter:
    """按调用 ID 配对工具事件，避免并行工具结束后覆盖仍在执行的命令。"""
    client_id = 1
    name = "codex"

    def __init__(self, *, reviewer_resolver=read_approval_reviewer, now=time.time):
        self._reviewer_resolver = reviewer_resolver
        self._now = now
        self._calls = {}
        self._turns = {}
        self._ended = set()

    def normalize(self, raw: dict) -> list[CcHudEvent]:
        event, p = raw.get("event", ""), raw.get("payload") or {}
        if p.get("model") == "codex-auto-review":
            return []  # 内部审批模型不是用户工作会话。
        if event not in self.hook_spec().events: return []
        sid, turn = p.get("session_id") or "default", p.get("turn_id")
        if turn and self._turns.get(sid) != turn:
            self._calls[sid] = {}
            self._turns[sid] = turn
            self._ended.discard(sid)
        if event == "PostToolUse" and sid in self._ended:
            return []
        calls = self._calls.setdefault(sid, {})
        tool = _TOOL_NAMES.get(p.get("tool_name", ""), p.get("tool_name", ""))
        call_id = p.get("tool_use_id") or tool or "unknown"
        state, detail, kind, phase = "thinking", "", None, "thinking"
        started = ended = None
        plan = None
        if event == "UserPromptSubmit":
            self._ended.discard(sid)
            calls.clear()
            started = self._now()
        elif event == "PreToolUse":
            self._ended.discard(sid)
            if tool in _WAIT_TOOLS:
                calls[call_id] = (tool, "question", "question")
            else:
                calls[call_id] = (tool, p.get("phase") or tool_phase(tool), None)
        elif event == "PostToolUse":
            calls.pop(call_id, None)
            detail = tool  # 思考时保留最近工具，界面明确标为“最近”。
            if isinstance(p.get("plan"), list): plan = p["plan"]
        elif event == "PermissionRequest":
            reviewer = self._reviewer_resolver(p)
            automatic = reviewer == "auto_review" or p.get("permission_mode") == "bypassPermissions"
            human = reviewer == "user" and not automatic
            logging.getLogger("cchud.codex").info(
                "审批事件：会话=%s，轮次=%s，处理方=%s，人工等待=%s",
                sid[:12], (turn or "未知")[:12], reviewer or "未知", human)
            if human:
                state, detail, kind, phase = "waiting", tool, "approval", "approval"
            elif not calls:
                phase = "review"  # 尚未开始执行命令，不伪装为运行中。
        elif event in ("Stop", "Interrupt", "SessionEnd"):
            calls.clear()
            self._ended.add(sid)
            state, phase, ended = "idle", "idle", self._now()
            detail = "turn-complete" if event == "Stop" else ""
        if calls and state != "waiting":
            pending = list(calls.values())
            selected = next((c for c in pending if c[2] == "question"), pending[-1])
            detail, phase, kind = selected
            state = "waiting" if kind else "tool"
        if detail.startswith("mcp__"):
            detail = detail.rsplit("__", 1)[-1]
        cwd = p.get("cwd")
        out = CcHudEvent(1, sid, "state", state=state, detail=detail,
                        intervention_kind=kind, turn_id=turn,
                        project=Path(cwd).name if isinstance(cwd, str) else None,
                        model=p.get("model") if isinstance(p.get("model"), str) else None,
                        phase=phase, plan=plan, started_at=started, ended_at=ended,
                        session_end=event == "SessionEnd", question_cancelled=event == "Interrupt",
                        transcript_path=p.get("transcript_path"),
                        attention=(p.get("attention") or f"确认是否允许 {detail or tool or '当前操作'}") if kind=="approval" else None,
                        outcome=(1 if event=="Stop" else 2) if event in ("Stop","Interrupt") else None)
        if event == "SessionEnd":
            self._calls.pop(sid, None)
            self._turns.pop(sid, None)
        return [out]

    def hook_spec(self) -> HookSpec:
        return HookSpec(events={name: f"codex {name}" for name in (
            "UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop",
            "PermissionRequest", "Interrupt", "SessionEnd")})
