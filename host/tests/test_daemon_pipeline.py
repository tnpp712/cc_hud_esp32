import pytest
from cchud_daemon.daemon import Daemon
from cchud_daemon.event import CcHudEvent


class SpyBle:
    def __init__(self): self.sent = []; self.use_v7 = False
    async def enqueue(self, payload): self.sent.append(bytes(payload))
    async def run(self): pass
    async def stop(self): pass


@pytest.mark.asyncio
async def test_state_event_pushes_0x07():
    ble = SpyBle()
    t = {"v": 1000.0}
    d = Daemon("addr", "/tmp/x.sock", ble=ble, now=lambda: t["v"])
    await d.on_events([CcHudEvent(0, "s1", "state", state="tool", detail="Bash")])
    assert ble.sent and ble.sent[0][0] == 0x07 and ble.sent[0][1] == 2


@pytest.mark.asyncio
async def test_quota_rate_limited():
    ble = SpyBle()
    t = {"v": 1000.0}
    d = Daemon("addr", "/tmp/x.sock", ble=ble, now=lambda: t["v"])
    q = CcHudEvent(0, "s1", "quota", five_h_used=40, five_h_resets_at=2000,
                   seven_d_used=28, seven_d_resets_at=9000, ctx_pct=65,
                   cost_usd=0.42, duration_s=23, lines_added=1, lines_removed=0,
                   title="Opus 4.8", mode="sub")
    await d.on_events([q])
    assert len(ble.sent) == 1                       # 首次推送
    t["v"] = 1010.0
    await d.on_events([q])
    assert len(ble.sent) == 1                       # 30s 内被限频
    t["v"] = 1040.0
    await d.on_events([q])
    assert len(ble.sent) == 2                       # 超 30s 再推
