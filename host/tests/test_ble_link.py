import asyncio
import pytest
from cchud_daemon.ble_link import BleLink

QUOTA_CHAR = "12345678-aaaa-bbbb-cccc-1234567890a1"


class FakeClient:
    """记录写入顺序、断言无并发写。"""
    instances = []

    def __init__(self, address, timeout=10):
        self.address = address
        self.writes = []
        self._in_write = False
        FakeClient.instances.append(self)

    async def __aenter__(self):
        return self

    async def __aexit__(self, *a):
        return False

    async def write_gatt_char(self, uuid, payload, response=True):
        assert not self._in_write, "并发写!应串行"
        self._in_write = True
        await asyncio.sleep(0.01)
        self.writes.append(bytes(payload))
        self._in_write = False


@pytest.mark.asyncio
async def test_serial_writes():
    FakeClient.instances.clear()
    link = BleLink("addr", client_factory=lambda addr, timeout: FakeClient(addr))
    task = asyncio.create_task(link.run())
    await link.enqueue(b"\x0b\x00\x00\x01")
    await link.enqueue(b"\x0b\x00\x00\x01\x01\x01\x28")
    await asyncio.sleep(0.1)
    await link.stop()
    task.cancel()
    writes = [w for c in FakeClient.instances for w in c.writes]
    assert len(writes) == 2
