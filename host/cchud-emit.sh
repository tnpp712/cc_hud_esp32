#!/usr/bin/env bash
# cchud-emit.sh — 瘦 hook 客户端:读 stdin,包成 JSON,投递给 daemon socket。
# 用法: cchud-emit.sh <client> <event>
# 失败一律静默退出 0,绝不阻塞调用方。
set -u
CLIENT="${1:-}"; EVENT="${2:-}"
SOCK="$HOME/.cchud/daemon.sock"
[ -S "$SOCK" ] || exit 0
STDIN="$(cat 2>/dev/null || true)"
# payload 直接透传 hook 的原始 stdin(JSON);daemon 侧 adapter 解析。
PAYLOAD="${STDIN:-\{\}}"
MSG="$(printf '{"client":"%s","event":"%s","payload":%s}' "$CLIENT" "$EVENT" "$PAYLOAD")"
# 投递一行给 daemon socket:优先 nc(轻量),失败再用 venv Python 兜底。
PYBIN="$(dirname "$0")/.venv/bin/python"
sent=0
if command -v nc >/dev/null 2>&1; then
    if printf '%s\n' "$MSG" | nc -U "$SOCK" -w 1 >/dev/null 2>&1; then
        sent=1
    fi
fi
if [ "$sent" != 1 ] && [ -x "$PYBIN" ]; then
    printf '%s\n' "$MSG" | "$PYBIN" -c \
      'import socket,sys,os;s=socket.socket(socket.AF_UNIX);s.connect(os.path.expanduser("~/.cchud/daemon.sock"));s.sendall(sys.stdin.buffer.read());s.close()' \
      >/dev/null 2>&1 || true
fi
exit 0
