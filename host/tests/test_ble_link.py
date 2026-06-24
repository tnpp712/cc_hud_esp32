import asyncio
import pytest
from cchud_daemon.ble_link import BleLink

QUOTA_CHAR = "12345678-aaaa-bbbb-cccc-1234567890a1"


class FakeClient:
    """记录写入顺序、断言无并发写。单实例复用(常驻连接)。"""
    _singleton: "FakeClient | None" = None
    instances: "list[FakeClient]" = []

    def __init__(self, address, timeout=10):
        self.address = address
        self.writes: list[bytes] = []
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


def _persistent_factory(addr, timeout):
    """工厂:每次返回同一个 FakeClient 实例,模拟常驻连接。"""
    if FakeClient._singleton is None:
        FakeClient._singleton = FakeClient(addr, timeout)
    return FakeClient._singleton


@pytest.mark.asyncio
async def test_serial_writes():
    """多次 enqueue 的写操作串行完成(无并发写)。"""
    FakeClient.instances.clear()
    FakeClient._singleton = None
    link = BleLink("addr", client_factory=lambda addr, timeout: FakeClient(addr))
    task = asyncio.create_task(link.run())
    await link.enqueue(b"\x0b\x00\x00\x01")
    await link.enqueue(b"\x0b\x00\x00\x01\x01\x01\x28")
    await asyncio.sleep(0.1)
    await link.stop()
    task.cancel()
    writes = [w for c in FakeClient.instances for w in c.writes]
    assert len(writes) == 2


@pytest.mark.asyncio
async def test_persistent_connection_reused():
    """常驻连接:多次 enqueue 使用同一个 client 实例,不重新建连接。"""
    FakeClient.instances.clear()
    FakeClient._singleton = None
    link = BleLink("addr", client_factory=_persistent_factory)
    task = asyncio.create_task(link.run())
    await link.enqueue(b"\x01\x00\x01")
    await link.enqueue(b"\x02\x00\x02")
    await link.enqueue(b"\x03\x00\x03")
    await asyncio.sleep(0.15)
    await link.stop()
    task.cancel()
    # 断言:只有一个 client 实例被创建(连接被复用)
    assert len(FakeClient.instances) == 1, \
        f"期望 1 个客户端实例(常驻连接),实际 {len(FakeClient.instances)} 个"
    # 断言:同一个 client 写入了全部 3 条 payload
    client = FakeClient.instances[0]
    assert len(client.writes) == 3, \
        f"期望 3 次写入,实际 {len(client.writes)} 次"
    assert client.writes == [b"\x01\x00\x01", b"\x02\x00\x02", b"\x03\x00\x03"]
