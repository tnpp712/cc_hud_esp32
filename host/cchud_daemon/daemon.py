from __future__ import annotations
import asyncio
import time
from .aggregator import Aggregator
from .ble_link import BleLink
from .codec import (encode_state_0x07, encode_quota_v6, encode_v7,
                    tlv_u8, tlv_u32, tlv_str, TAG_FIVE_H_USED_PCT,
                    TAG_FIVE_H_RESET_IN_S, TAG_SEVEN_D_USED_PCT,
                    TAG_SEVEN_D_RESET_IN_S, TAG_CONTEXT_USED_PCT,
                    TAG_COST_MICRO_USD, TAG_DURATION_S, TAG_LINES_ADDED,
                    TAG_LINES_REMOVED, TAG_PLAN_MODE, TAG_TITLE)
from .event import CcHudEvent
from .quota_tracker import QuotaTracker
from .registry import AdapterRegistry
from .adapters.claude import ClaudeAdapter
from .session_store import SessionStore
from .socket_server import SocketServer

_STATE_CODE = {"idle": 0, "thinking": 1, "tool": 2, "waiting": 3, "done": 4}


class Daemon:
    """组装全部前序模块,把 SocketServer→AdapterRegistry→SessionStore/QuotaTracker/Aggregator→BleLink 串成状态管线。"""

    def __init__(self, address: str, sock_path: str, *, ble=None,
                 now=time.time) -> None:
        self._now = now
        self._store = SessionStore()
        self._quota = QuotaTracker()
        self._agg = Aggregator()
        self._ble = ble if ble is not None else BleLink(address)
        self._registry = AdapterRegistry()
        self._registry.register(ClaudeAdapter())
        self._sock = SocketServer(sock_path, self.on_events, self._registry)
        self._last_title = "CC HUD"

    async def on_events(self, evs: list[CcHudEvent]) -> None:
        """分发事件:state 走状态管线,quota 走额度管线。"""
        for e in evs:
            if e.kind == "state":
                await self._handle_state(e)
            elif e.kind == "quota":
                await self._handle_quota(e)

    async def _handle_state(self, e: CcHudEvent) -> None:
        """state 事件 → SessionStore.update → Aggregator 聚合,变更则推送 0x07。"""
        now = self._now()
        self._store.update(e.session_id, e.state or "idle", e.detail or "", now)
        state, detail, total, busy = self._agg.aggregate(self._store, now)
        if self._agg.changed((state, detail, total, busy)):
            await self._ble.enqueue(
                encode_state_0x07(_STATE_CODE.get(state, 0), detail, total, busy))

    async def _handle_quota(self, e: CcHudEvent) -> None:
        """quota 事件 → QuotaTracker.merge 分窗合并 → should_push 为真则编码推送。"""
        now = self._now()
        used5, reset5_at = self._quota.merge(
            e.five_h_used or 0, e.five_h_resets_at or 0, "5h", now)
        used7, reset7_at = self._quota.merge(
            e.seven_d_used or 0, e.seven_d_resets_at or 0, "7d", now)
        if not self._quota.should_push(now):
            return
        self._quota.mark_pushed(now)
        if e.title:
            self._last_title = e.title
        reset5 = max(0, reset5_at - int(now)) if reset5_at else 0
        reset7 = max(0, reset7_at - int(now)) if reset7_at else 0
        cost_micro = max(0, int(round((e.cost_usd or 0) * 1_000_000)))
        if getattr(self._ble, "use_v7", False):
            payload = encode_v7([
                tlv_u8(TAG_PLAN_MODE, 0 if (e.mode or "sub") == "sub" else 1),
                tlv_u8(TAG_FIVE_H_USED_PCT, used5),
                tlv_u32(TAG_FIVE_H_RESET_IN_S, reset5),
                tlv_u8(TAG_SEVEN_D_USED_PCT, used7),
                tlv_u32(TAG_SEVEN_D_RESET_IN_S, reset7),
                tlv_u8(TAG_CONTEXT_USED_PCT, e.ctx_pct or 0),
                tlv_u32(TAG_COST_MICRO_USD, cost_micro),
                tlv_u32(TAG_DURATION_S, e.duration_s or 0),
                tlv_u32(TAG_LINES_ADDED, e.lines_added or 0),
                tlv_u32(TAG_LINES_REMOVED, e.lines_removed or 0),
                tlv_str(TAG_TITLE, self._last_title),
            ])
        else:
            payload = encode_quota_v6(
                mode=0 if (e.mode or "sub") == "sub" else 1,
                h5_used=used5, h5_limit=100, d7_used=used7, d7_limit=100,
                h5_reset_s=reset5, d7_reset_s=reset7,
                cost_micro_usd=cost_micro, duration_s=e.duration_s or 0,
                ctx_pct=e.ctx_pct or 0, lines_added=e.lines_added or 0,
                lines_removed=e.lines_removed or 0, title=self._last_title)
        await self._ble.enqueue(payload)

    async def start(self) -> None:
        """启动 socket 监听和 BLE 运行循环。"""
        await self._sock.start()
        asyncio.create_task(self._ble.run())

    async def stop(self) -> None:
        """停止 socket 服务和 BLE。"""
        await self._sock.stop()
        await self._ble.stop()
