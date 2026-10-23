"""只保留 HUD 需要的事件元数据，避免把命令正文或输出送入 socket。"""
from __future__ import annotations
import re


def compact_payload(payload: dict) -> dict:
    keep = ("session_id", "turn_id", "transcript_path", "tool_name", "tool_use_id",
            "permission_mode", "approvals_reviewer", "cwd", "model")
    out = {key: payload[key] for key in keep if key in payload}
    tool_input = payload.get("tool_input") or {}
    if not isinstance(tool_input, dict):
        tool_input = {}
    # 只有结构化计划才进入步骤信息，命令字符串不进入上报。
    if str(payload.get("tool_name", "")).split(".")[-1] == "update_plan":
        plan = tool_input.get("plan")
        if isinstance(plan, list):
            out["plan"] = [{"step": str(p.get("step", ""))[:80], "status": {"inProgress": "in_progress"}.get(p.get("status"), p.get("status"))}
                           for p in plan[:50] if isinstance(p, dict)]
    command = tool_input.get("command", tool_input.get("cmd", ""))
    if isinstance(command, str) and re.search(r"(?:^|[\s;&|/])(pytest|ctest|jest|vitest)(?:\s|$)|\b(?:npm|pnpm|cargo|pio)\s+test\b", command):
        out["phase"] = "test"
    return out
