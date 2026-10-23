import json
import os
from pathlib import Path
import socket
import subprocess
import threading


def test_codex_hook_strips_large_tool_payload_and_stop_returns_json(tmp_path):
    home = tmp_path
    state = home / '.cchud'
    state.mkdir()
    path = str(state / 'daemon.sock')
    server = socket.socket(socket.AF_UNIX)
    server.bind(path)
    server.listen(1)
    received = []
    def listen():
        with server.accept()[0] as client:
            data = b''
            while b'\n' not in data:
                data += client.recv(4096)
            received.append(json.loads(data))
    thread = threading.Thread(target=listen)
    thread.start()
    script = Path(__file__).resolve().parents[1] / 'cchud-emit.sh'
    result = subprocess.run([str(script), 'codex', 'Stop'],
        input=json.dumps({'session_id':'s','tool_name':'Bash','tool_response':'x'*100000}),
        text=True, capture_output=True, timeout=3, env={**os.environ,'CCHUD_SOCKET':path})
    thread.join(timeout=3)
    server.close()
    assert result.returncode == 0 and json.loads(result.stdout) == {}
    assert received[0]['payload'] == {'session_id':'s','tool_name':'Bash'}
