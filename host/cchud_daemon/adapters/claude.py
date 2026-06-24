from __future__ import annotations
from ..event import CcHudEvent
from .base import HookSpec

# 复刻 cchud-hook.sh 的 Notification 降级关键词
_PERMISSION_HINTS = ("permission", "Permission", "approve", "Approve",
                     "needs your", "confirm", "Confirm")

_EVENT_STATE = {
    "UserPromptSubmit": "thinking",
    "PostToolUse": "thinking",
    "PreToolUse": "tool",
    "Stop": "idle",
}

# 这些工具实为向用户提问(等回答),应映射为 waiting 而非普通工具运行
_WAIT_TOOLS = {"AskUserQuestion"}


def _fmt_ctx_size(n: int) -> str:
    if n >= 1_000_000:
        return f"{n // 1_000_000}M"
    if n >= 1000:
        return f"{n // 1000}K"
    return ""


class ClaudeAdapter:
    client_id = 0
    name = "claude"

    def normalize(self, raw: dict) -> list[CcHudEvent]:
        event = raw.get("event", "")
        payload = raw.get("payload", {})
        sid = payload.get("session_id") or "default"
        out: list[CcHudEvent] = []

        if event in _EVENT_STATE:
            state = _EVENT_STATE[event]
            detail = payload.get("tool_name", "") if state == "tool" else ""
            kind = None
            if state == "tool" and detail in _WAIT_TOOLS:
                state, kind = "waiting", "question"   # AskUserQuestion 等用户回答
            out.append(CcHudEvent(self.client_id, sid, "state",
                                  state=state, detail=detail,
                                  intervention_kind=kind))
        elif event == "Notification":
            msg = payload.get("message", "") or ""
            if any(h in msg for h in _PERMISSION_HINTS):
                out.append(CcHudEvent(self.client_id, sid, "state",
                                      state="waiting",
                                      intervention_kind="approval"))
            else:
                out.append(CcHudEvent(self.client_id, sid, "state", state="idle"))

        if event == "Status":
            # statusLine 透传:payload 本身就是 statusline JSON
            out.append(self._quota_from_statusline(sid, payload))
        return out

    def _quota_from_statusline(self, sid: str, sl: dict) -> CcHudEvent:
        rl = sl.get("rate_limits", {})
        fh = rl.get("five_hour", {})
        sd = rl.get("seven_day", {})
        cw = sl.get("context_window", {})
        cost = sl.get("cost", {})
        model = (sl.get("model", {}) or {}).get("display_name", "") or ""
        ctx_size = int(cw.get("context_window_size", 0) or 0)
        ctx_txt = _fmt_ctx_size(ctx_size)
        if model and ctx_txt:
            title = f"{model} ({ctx_txt} context)"
        elif model:
            title = model
        else:
            title = None
        return CcHudEvent(
            self.client_id, sid, "quota",
            five_h_used=int(fh.get("used_percentage", 0) or 0),
            five_h_resets_at=int(fh.get("resets_at", 0) or 0),
            seven_d_used=int(sd.get("used_percentage", 0) or 0),
            seven_d_resets_at=int(sd.get("resets_at", 0) or 0),
            ctx_pct=int(cw.get("used_percentage", 0) or 0),
            cost_usd=float(cost.get("total_cost_usd", 0) or 0),
            duration_s=int((cost.get("total_duration_ms", 0) or 0)) // 1000,
            lines_added=int(cost.get("total_lines_added", 0) or 0),
            lines_removed=int(cost.get("total_lines_removed", 0) or 0),
            title=title, mode="sub")

    def hook_spec(self) -> HookSpec:
        return HookSpec(events={
            "UserPromptSubmit": "claude UserPromptSubmit",
            "PreToolUse": "claude PreToolUse",
            "PostToolUse": "claude PostToolUse",
            "Stop": "claude Stop",
            "Notification": "claude Notification",
        }, statusline_wrapper="cchud-statusline.sh")
