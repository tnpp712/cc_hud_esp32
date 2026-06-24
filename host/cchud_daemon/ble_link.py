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

    async def _write_one(self, payload: bytes) -> None:
        last = None
        for _ in range(2):                       # 重试 1 次
            try:
                async with self._factory(self._address, self._timeout) as c:
                    await c.write_gatt_char(QUOTA_CHAR, payload, response=True)
                return
            except Exception as exc:             # noqa: BLE001
                last = exc
                await asyncio.sleep(0.4)
        _log.warning("BLE 写入失败: %s", last)

    async def run(self) -> None:
        backoff = 0.5
        while not self._stop.is_set():
            try:
                payload = await asyncio.wait_for(self._queue.get(), timeout=1.0)
            except asyncio.TimeoutError:
                continue
            await self._write_one(payload)        # 串行:一次一个
            backoff = 0.5

    async def stop(self) -> None:
        self._stop.set()
