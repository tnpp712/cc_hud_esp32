from __future__ import annotations
import asyncio
import logging
from .codec import decode_work_readback

QUOTA_CHAR = "12345678-aaaa-bbbb-cccc-1234567890a1"
WORK_READBACK_CHAR = "12345678-aaaa-bbbb-cccc-1234567890a4"
_log = logging.getLogger("cchud.ble")


def _default_factory(address: str, timeout: float):
    from bleak import BleakClient
    return BleakClient(address, timeout=timeout)


class BleLink:
    """常驻 BLE 连接 + 串行发送队列(取代 ble.lock + 每次重连)。"""

    def __init__(self, address: str, *, client_factory=_default_factory,
                 use_v7: bool = False, timeout: float = 8.0) -> None:
        self._address = address
        self._factory = client_factory
        self._timeout = timeout
        self.use_v7 = use_v7
        self._queue: asyncio.Queue[bytes] = asyncio.Queue()
        self._stop = asyncio.Event()
        self.connected = False
        self._snapshot = None
        self.device_work = None

    async def enqueue(self, payload: bytes) -> None:
        await self._queue.put(payload)

    async def enqueue_snapshot(self, frames: list[bytes]) -> None:
        # 尚未开始发送的工作页只保留最新一份；已发送的代次必须完整结束。
        self._snapshot = list(frames)

    async def run(self) -> None:
        backoff = 0.5
        pending = None
        batch = []
        while not self._stop.is_set():
            try:
                async with self._factory(self._address, self._timeout) as client:
                    self.connected = True
                    self.device_work = None
                    _log.info("BLE 已连接：%s", self._address)
                    backoff = 0.5                      # 连上即重置退避
                    while not self._stop.is_set():
                        try:
                            if pending is not None:
                                payload = pending
                            elif batch:
                                payload = batch.pop(0)
                            elif not self._queue.empty():
                                payload = self._queue.get_nowait()
                            elif self._snapshot is not None:
                                batch, self._snapshot = self._snapshot, None
                                if not batch:
                                    continue
                                payload = batch.pop(0)
                            else:
                                payload = await asyncio.wait_for(self._queue.get(), timeout=0.2)
                        except asyncio.TimeoutError:
                            if not getattr(client, "is_connected", True):
                                break
                            continue
                        try:
                            await client.write_gatt_char(QUOTA_CHAR, payload, response=True)
                            pending = None
                            if not batch and payload[0] == 0x0B and bytes([0x63, 4]) in payload[:8]:
                                services = getattr(client, "services", None)
                                if services and services.get_characteristic(WORK_READBACK_CHAR):
                                    self.device_work = decode_work_readback(bytes(await client.read_gatt_char(WORK_READBACK_CHAR)))
                        except asyncio.CancelledError:
                            raise
                        except Exception as exc:
                            pending = payload  # 写失败后保留该帧，重连再投递。
                            _log.warning("BLE 写入失败,将重连: %s", exc)
                            break
            except asyncio.CancelledError:
                raise
            except Exception as exc:                    # 连接失败→退避后重试
                _log.warning("BLE 连接失败: %s", exc)
            finally:
                self.connected = False
            if not self._stop.is_set():
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 30.0)

    async def stop(self) -> None:
        self._stop.set()
