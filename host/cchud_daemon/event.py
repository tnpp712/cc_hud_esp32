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
    # 介入类型(state=waiting 时):None/"approval"(等批准)/"question"(等回答)/"error"
    intervention_kind: str | None = None
    ts: float = 0.0
    # v7 扩展：5H、7D、上下文、费用、时长、行数是否已知；None 兼容旧客户端。
    metrics_valid: int | None = None
    turn_id: str | None = None
    project: str | None = None
    model: str | None = None
    effort: str | None = None
    phase: str | None = None
    plan: list | None = None
    started_at: float | None = None
    ended_at: float | None = None
    session_end: bool = False
    transcript_path: str | None = None
    pending_questions: int | None = None
    question_ids: list[str] | None = None
    question_cancelled: bool = False
