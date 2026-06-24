from __future__ import annotations
import asyncio
import json
import os
from typing import Awaitable, Callable
from .event import CcHudEvent
from .registry import AdapterRegistry

OnEvents = Callable[[list[CcHudEvent]], Awaitable[None]]


class SocketServer:
    """监听 Unix domain socket,把 hook 投递的 JSON 规范化为事件。"""

    def __init__(self, path: str, on_events: OnEvents, registry: AdapterRegistry) -> None:
        self._path = path
        self._on_events = on_events
        self._registry = registry
        self._server: asyncio.AbstractServer | None = None

    async def _handle(self, reader: asyncio.StreamReader,
                      writer: asyncio.StreamWriter) -> None:
        try:
            line = await reader.readline()
            if not line:
                return
            raw = json.loads(line.decode("utf-8"))
            evs = self._registry.normalize(raw)
            if evs:
                await self._on_events(evs)
        except (json.JSONDecodeError, UnicodeDecodeError):
            pass                                  # 脏行忽略,不崩溃
        finally:
            writer.close()
            await writer.wait_closed()

    async def start(self) -> None:
        if os.path.exists(self._path):
            os.unlink(self._path)
        parent = os.path.dirname(self._path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        self._server = await asyncio.start_unix_server(self._handle, path=self._path)

    async def stop(self) -> None:
        if self._server:
            self._server.close()
            await self._server.wait_closed()
        if os.path.exists(self._path):
            os.unlink(self._path)
