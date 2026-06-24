from __future__ import annotations
from ..event import CcHudEvent
from .base import HookSpec

# Codex CLI 的 hook 事件名为 PascalCase(同 Claude),配置在 ~/.codex/hooks.json。
# 与 Claude 不同:权限等待是独立的 PermissionRequest 事件(Claude 用 Notification
# + 关键词降级);Codex 无 statusline 额度来源,本 adapter 只产 state 事件。
_EVENT_STATE = {
    "UserPromptSubmit": "thinking",
    "PostToolUse": "thinking",
    "PreToolUse": "tool",
    "Stop": "idle",
}

# 实测发现:Codex(CLI/App)用一个名为 request_user_input 的「工具调用」来向
# 用户提问(走 PreToolUse 事件),而非 PermissionRequest。命中这些工具名时,
# 应表达为「等待用户」而不是普通工具运行。
_WAIT_TOOLS = {"request_user_input"}


class CodexAdapter:
    """把 Codex CLI 的 hook 事件规范化为 CcHudEvent(状态;MVP 不含额度)。"""

    client_id = 1
    name = "codex"

    def normalize(self, raw: dict) -> list[CcHudEvent]:
        event = raw.get("event", "")
        payload = raw.get("payload", {})
        sid = payload.get("session_id") or "default"
        out: list[CcHudEvent] = []

        tool = payload.get("tool_name", "")
        if event in _EVENT_STATE:
            state = _EVENT_STATE[event]
            detail = ""
            if event == "PreToolUse":
                if tool in _WAIT_TOOLS:
                    state = "waiting"          # Codex 通过工具调用问用户 → 等待态
                else:
                    detail = tool
            out.append(CcHudEvent(self.client_id, sid, "state",
                                  state=state, detail=detail))
        elif event == "PermissionRequest":
            # 等待用户批准工具调用 → waiting,detail 带工具名
            out.append(CcHudEvent(self.client_id, sid, "state",
                                  state="waiting",
                                  detail=payload.get("tool_name", "")))
        return out

    def hook_spec(self) -> HookSpec:
        return HookSpec(events={
            "UserPromptSubmit": "codex UserPromptSubmit",
            "PreToolUse": "codex PreToolUse",
            "PostToolUse": "codex PostToolUse",
            "Stop": "codex Stop",
            "PermissionRequest": "codex PermissionRequest",
        })
