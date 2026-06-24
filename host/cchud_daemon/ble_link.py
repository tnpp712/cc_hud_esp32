from __future__ import annotations
import asyncio
import logging

QUOTA_CHAR = "12345678-aaaa-bbbb-cccc-1234567890a1"
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

    async def enqueue(self, payload: bytes) -> None:
        await self._queue.put(payload)

    async def run(self) -> None:
        backoff = 0.5
        while not self._stop.is_set():
            try:
                async with self._factory(self._address, self._timeout) as client:
                    backoff = 0.5                      # 连上即重置退避
                    while not self._stop.is_set():
                        try:
                            payload = await asyncio.wait_for(self._queue.get(), timeout=1.0)
                        except asyncio.TimeoutError:
                            continue
                        try:
                            await client.write_gatt_char(QUOTA_CHAR, payload, response=True)
                        except asyncio.CancelledError:
                            raise
                        except Exception as exc:        # 写失败→跳出重连;该帧丢弃,后续状态会刷新
                            _log.warning("BLE 写入失败,将重连: %s", exc)
                            break
            except asyncio.CancelledError:
                raise
            except Exception as exc:                    # 连接失败→退避后重试
                _log.warning("BLE 连接失败: %s", exc)
            if not self._stop.is_set():
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 30.0)

    async def stop(self) -> None:
        self._stop.set()
