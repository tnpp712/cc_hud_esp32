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
