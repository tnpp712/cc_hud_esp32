from __future__ import annotations
from .adapters.base import Adapter
from .event import CcHudEvent


class AdapterRegistry:
    def __init__(self) -> None:
        self._by_name: dict[str, Adapter] = {}

    def register(self, a: Adapter) -> None:
        self._by_name[a.name] = a

    def normalize(self, raw: dict) -> list[CcHudEvent]:
        a = self._by_name.get(raw.get("client", ""))
        return a.normalize(raw) if a else []
