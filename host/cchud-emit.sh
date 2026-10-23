#!/usr/bin/env bash
# 只向本机 daemon 投递事件；失败静默，不阻塞 Codex 或输出会话正文。
set -u
CLIENT="${1:-}"
EVENT="${2:-}"
SOCK="${CCHUD_SOCKET:-$HOME/.cchud/daemon.sock}"
PYBIN="$(dirname "$0")/.venv/bin/python"
# Codex Stop 要求 JSON；即使 daemon 未运行也返回合法结果。
trap 'if [ "$CLIENT" = codex ] && [ "$EVENT" = Stop ]; then printf "{}\n"; fi' EXIT
[ -S "$SOCK" ] && [ -x "$PYBIN" ] || exit 0
PYTHONPATH="$(dirname "$0")${PYTHONPATH:+:$PYTHONPATH}" "$PYBIN" -c '
import json,socket,sys
try:
    client,event,path=sys.argv[1:]
    payload=json.load(sys.stdin)
    if client=="codex":
        from cchud_daemon.hook_payload import compact_payload
        payload=compact_payload(payload)
    data=json.dumps({"client":client,"event":event,"payload":payload},ensure_ascii=False).encode()+b"\n"
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(0.5)
        s.connect(path)
        s.sendall(data)
except (OSError,ValueError,TypeError):
    pass
' "$CLIENT" "$EVENT" "$SOCK" >/dev/null 2>&1 || true
exit 0
