from __future__ import annotations
from dataclasses import dataclass, field
from typing import Protocol
from ..event import CcHudEvent


@dataclass
class HookSpec:
    events: dict[str, str] = field(default_factory=dict)   # 事件名 -> emit 参数
    statusline_wrapper: str | None = None                  # statusLine 包装脚本文件名(同 emit 目录)


class Adapter(Protocol):
    client_id: int
    name: str
    def normalize(self, raw: dict) -> list[CcHudEvent]: ...
    def hook_spec(self) -> HookSpec: ...
