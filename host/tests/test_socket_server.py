import asyncio
import json
import pytest
from cchud_daemon.registry import AdapterRegistry
from cchud_daemon.adapters.claude import ClaudeAdapter
from cchud_daemon.socket_server import SocketServer


@pytest.mark.asyncio
async def test_socket_roundtrip(tmp_path):
    received = []

    async def on_events(evs):
        received.extend(evs)

    reg = AdapterRegistry(); reg.register(ClaudeAdapter())
    sock = str(tmp_path / "d.sock")
    srv = SocketServer(sock, on_events, reg)
    await srv.start()
    try:
        r, w = await asyncio.open_unix_connection(sock)
        w.write((json.dumps({"client": "claude", "event": "PreToolUse",
                             "payload": {"session_id": "s1", "tool_name": "Bash"}}) + "\n").encode())
        await w.drain(); w.close()
        await asyncio.sleep(0.1)
    finally:
        await srv.stop()
    assert any(e.detail == "Bash" for e in received)


@pytest.mark.asyncio
async def test_status_is_read_only(tmp_path):
    import json
    received=[]
    async def on_events(events):received.extend(events)
    reg=AdapterRegistry()
    srv=SocketServer(str(tmp_path/'status.sock'),on_events,reg,lambda:{'ble_connected':True,'sessions':[]})
    await srv.start()
    try:
        reader,writer=await asyncio.open_unix_connection(str(tmp_path/'status.sock'))
        writer.write(b'{"op":"status"}\n');await writer.drain()
        assert json.loads(await reader.readline())=={'ble_connected':True,'sessions':[]}
        writer.close();await writer.wait_closed()
        assert received==[]
    finally:await srv.stop()
