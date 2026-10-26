from __future__ import annotations
import asyncio
import time
import logging
import os
from pathlib import Path
from .codex_usage import CodexUsageReader
from .aggregator import Aggregator
from .ble_link import BleLink
from .codec import (encode_state_0x07, encode_v7_state, encode_time_v4, encode_quota_v6, encode_v7, KIND_CODE,
                    tlv_u8, tlv_u32, tlv_str, TAG_FIVE_H_USED_PCT,
                    TAG_FIVE_H_RESET_IN_S, TAG_SEVEN_D_USED_PCT,
                    TAG_SEVEN_D_RESET_IN_S, TAG_CONTEXT_USED_PCT,
                    TAG_COST_MICRO_USD, TAG_DURATION_S, TAG_LINES_ADDED,
                    TAG_LINES_REMOVED, TAG_PLAN_MODE, TAG_TITLE)
from .event import CcHudEvent
from .codec import encode_work_frames
from .quota_tracker import QuotaTracker
from .registry import AdapterRegistry
from .adapters.claude import ClaudeAdapter
from .adapters.codex import CodexAdapter
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
        self._registry.register(CodexAdapter())
        self._sock = SocketServer(sock_path, self.on_events, self._registry, self.status)
        self._last_title = "Codex"
        self._tasks = []
        self._pending_usage = None
        self._work_key = None
        self._completed_turns = {}
        self._work_sent = 0
        self._generation = int(self._now() * 1000) & 0xFFFFFFFF
        self._reader = CodexUsageReader(Path(os.environ.get("CODEX_HOME", "~/.codex")).expanduser(),
                                       closure_path=Path(sock_path).with_suffix(".questions.json"),
                                       sources_path=Path(sock_path).with_suffix(".sources.json"))

    def status(self):
        return {"ble_connected": bool(getattr(self._ble, "connected", False)),
                "device_work": getattr(self._ble, "device_work", None),
                "work_generation": self._generation, "work_sent_at": self._work_sent,
                "sessions": self._store.work_rows(self._now())}

    async def on_events(self, evs: list[CcHudEvent]) -> None:
        """分发事件:state 走状态管线,quota 走额度管线。"""
        for e in evs:
            if e.kind == "state":
                await self._handle_state(e)
            elif e.kind == "quota":
                await self._handle_quota(e)
            elif e.kind == "metadata":
                self._store.apply(e, self._now())

    async def _handle_state(self, e: CcHudEvent) -> None:
        """state 事件 → SessionStore.update → Aggregator 聚合,变更则推送 0x07。"""
        now = self._now()
        if e.session_end: self._reader.untrack_path(e.transcript_path)
        else: self._reader.track_path(e.transcript_path)
        if e.session_end or e.question_cancelled:
            known_ids = set(self._store.pending_ids(e.session_id))
            self._reader.questions.cancel(e.session_id,e.turn_id,now,session_end=e.session_end,persist=True)
            e.question_ids=[qid for qid in self._reader.questions.ids(e.session_id) if qid in known_ids]
            e.pending_questions=len(e.question_ids)
        self._store.apply(e, now)
        await self._emit_state(now)
        completion=(e.session_id,e.turn_id or e.ended_at)
        fresh=e.detail=="turn-complete" and completion not in self._completed_turns
        if fresh:
            self._completed_turns[completion]=now
            self._completed_turns={k:v for k,v in self._completed_turns.items() if now-v<86400}
        if fresh and self._store.counts(now)[1] == 0:
            # 完成脉冲只在所有会话空闲时发送，不能覆盖另一会话的工作或审批状态。
            total, busy = self._store.counts(now)
            payload = (encode_v7_state(4, "", total, busy, "none")
                       if getattr(self._ble, "use_v7", False)
                       else encode_state_0x07(4, "", total, busy))
            await self._ble.enqueue(payload)

    async def _emit_work(self, now):
        if not getattr(self._ble, "use_v7", False) or now - self._work_sent < 1:
            return
        rows = self._store.work_rows(now)[:8]
        key = repr([{k: v for k, v in row.items() if k not in ("elapsed", "last_seen")} for row in rows])
        if key == self._work_key and now - self._work_sent < 10:
            return
        self._work_key, self._work_sent = key, now
        self._generation = (self._generation + 1) & 0xFFFFFFFF
        frames = encode_work_frames(rows, self._generation)
        if hasattr(self._ble, "enqueue_snapshot"):
            await self._ble.enqueue_snapshot(frames)
        else:
            for frame in frames:
                await self._ble.enqueue(frame)

    async def _emit_state(self, now: float) -> None:
        """重新聚合并(仅在变化时)推送状态帧。被状态事件与周期 tick 共用。"""
        state, detail, kind, total, busy = self._agg.aggregate(self._store, now)
        # 第 4 层:会话列表(限前 6 条保证单包),已按优先级排序(等你的在前)
        sessions = [
            (cid, _STATE_CODE.get(st, 0), KIND_CODE.get(k, 0), (det or "")[:20])
            for (cid, st, det, k) in self._store.session_list(now)[:6]
        ]
        key = (state, detail, kind, total, busy, tuple(sessions))
        if self._agg.changed(key):
            code = _STATE_CODE.get(state, 0)
            if getattr(self._ble, "use_v7", False):
                payload = encode_v7_state(code, detail, total, busy, kind,
                                          sessions=sessions)
            else:
                payload = encode_state_0x07(code, detail, total, busy)
            await self._ble.enqueue(payload)

    async def _tick_loop(self) -> None:
        """周期重聚合:TTL 过期会话被清后,设备自动回 idle(自愈静默结束的会话)。"""
        while True:
            try:
                await self._emit_state(self._now())   # 启动即推一次,清除上次残留态
                await self._emit_work(self._now())
            except Exception:  # noqa: BLE001
                logging.getLogger("cchud.state").exception("聚合 HUD 状态失败")
            await asyncio.sleep(1)

    async def _handle_quota(self, e: CcHudEvent) -> None:
        """quota 事件 → QuotaTracker.merge 分窗合并 → should_push 为真则编码推送。"""
        now = self._now()
        if e.client_id == 1:
            # Codex 是完整快照；未知窗口不得被旧客户端的额度填充。
            used5, reset5_at = 0, 0
            used7, reset7_at = e.seven_d_used or 0, e.seven_d_resets_at or 0
        else:
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
            fields = [
                tlv_u8(TAG_PLAN_MODE, 0 if (e.mode or "sub") == "sub" else 1),
                tlv_u8(TAG_FIVE_H_USED_PCT, used5),
                tlv_u32(TAG_FIVE_H_RESET_IN_S, reset5),
                tlv_u8(TAG_SEVEN_D_USED_PCT, used7),
                tlv_u32(TAG_SEVEN_D_RESET_IN_S, reset7),
                tlv_u8(TAG_CONTEXT_USED_PCT, max(0, min(100, e.ctx_pct or 0))),
                tlv_u32(TAG_COST_MICRO_USD, cost_micro),
                tlv_u32(TAG_DURATION_S, e.duration_s or 0),
                tlv_u32(TAG_LINES_ADDED, e.lines_added or 0),
                tlv_u32(TAG_LINES_REMOVED, e.lines_removed or 0),
                tlv_str(TAG_TITLE, self._last_title),
            ]
            if e.metrics_valid is not None:
                fields.append(tlv_u8(0x0C, e.metrics_valid & ~1))
            if e.client_id == 1:
                fields = [field for field in fields if field[0] not in
                          (TAG_FIVE_H_USED_PCT, TAG_FIVE_H_RESET_IN_S)]
            payload = encode_v7(fields)
        else:
            payload = encode_quota_v6(
                mode=0 if (e.mode or "sub") == "sub" else 1,
                h5_used=used5, h5_limit=100, d7_used=used7, d7_limit=100,
                h5_reset_s=reset5, d7_reset_s=reset7,
                cost_micro_usd=cost_micro, duration_s=e.duration_s or 0,
                ctx_pct=e.ctx_pct or 0, lines_added=e.lines_added or 0,
                lines_removed=e.lines_removed or 0, title=self._last_title)
        await self._ble.enqueue(payload)

    async def _codex_usage_loop(self) -> None:
        reader = self._reader
        def metadata(events):
            rows=[e for e in events if e.kind == "metadata"]
            # 读取线程返回后再次取当前问题快照，避免与实时中断事件交错时恢复旧提醒。
            for event in rows:
                event.pending_questions=reader.questions.count(event.session_id)
                event.question_ids=reader.questions.ids(event.session_id)
                if event.pending_questions:
                    event.attention=reader.questions.attention(event.session_id)
                elif event.attention:
                    event.attention=""
            return rows
        # 优先显示新鲜 Codex 快照；没有数据时明确清除旧客户端残留。
        try:
            initial = await asyncio.to_thread(reader.poll)
        except Exception:
            logging.getLogger("cchud.codex").exception("首次读取 Codex 元数据失败，将重试")
            initial = []
        await self.on_events(metadata(initial))
        quotas = [e for e in initial if e.kind == "quota"]
        await self._handle_quota(quotas[-1] if quotas else
                                 CcHudEvent(1, "usage", "quota", title="Codex", metrics_valid=0))
        while True:
            try:
                events = await asyncio.to_thread(reader.poll)
                await self.on_events(metadata(events))
                quotas = [e for e in events if e.kind == "quota"]
                if quotas:
                    self._pending_usage = (quotas[-1], self._now())
                if self._pending_usage and self._quota.should_push(self._now()):
                    event, received = self._pending_usage
                    if self._now() - received <= 60:
                        await self._handle_quota(event)
                    self._pending_usage = None
            except Exception:
                logging.getLogger("cchud.codex").exception("读取 Codex 用量失败")
            await asyncio.sleep(2)

    def _fetch_weather_sync(self) -> str:
        """复用 push_idle.fetch_weather 获取中文天气;失败返回空串。"""
        import os
        import sys
        host_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        if host_dir not in sys.path:
            sys.path.insert(0, host_dir)
        from push_idle import fetch_weather
        return fetch_weather(os.environ.get("CCHUD_WEATHER_CITY", "北京")) or ""

    async def _push_time(self) -> None:
        """推送 v4 时间帧:校准设备 RTC(无网无 NTP 时唯一时间源)+ 空闲页天气。
        接管旧 push_idle.py 的职责(第 1 层 daemon 重构遗漏的)。"""
        now = int(self._now())
        # 本机 UTC 偏移(分钟),处理夏令时
        is_dst = time.localtime(now).tm_isdst > 0
        off_sec = -(time.altzone if is_dst else time.timezone)
        # 天气获取走线程,避免阻塞事件循环;失败则只推时间(空 status)
        status = ""
        try:
            status = await asyncio.to_thread(self._fetch_weather_sync)
        except Exception:  # noqa: BLE001
            status = ""
        await self._ble.enqueue(encode_time_v4(now, off_sec // 60, status))

    async def _push_time_loop(self) -> None:
        """启动即推一次,之后每 30 分钟校准一次。"""
        while True:
            try:
                await self._push_time()
            except Exception:  # noqa: BLE001 — 时间推送失败不影响主流程
                pass
            await asyncio.sleep(1800)

    async def start(self) -> None:
        """启动 socket 监听、BLE 运行循环和周期时间校准。"""
        await self._sock.start()
        self._tasks = [asyncio.create_task(loop()) for loop in
                       (self._ble.run, self._push_time_loop, self._tick_loop, self._codex_usage_loop)]

    async def stop(self) -> None:
        """停止 socket 服务和 BLE。"""
        await self._sock.stop()
        await self._ble.stop()
        for task in self._tasks:
            task.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
