from __future__ import annotations
from dataclasses import dataclass


@dataclass
class CcHudEvent:
    client_id: int
    session_id: str
    kind: str                       # "state" | "quota" | "idle"
    state: str | None = None
    detail: str | None = None
    five_h_used: int | None = None
    five_h_resets_at: int | None = None
    seven_d_used: int | None = None
    seven_d_resets_at: int | None = None
    ctx_pct: int | None = None
    cost_usd: float | None = None
    duration_s: int | None = None
    lines_added: int | None = None
    lines_removed: int | None = None
    title: str | None = None
    mode: str | None = None
    ts: float = 0.0
