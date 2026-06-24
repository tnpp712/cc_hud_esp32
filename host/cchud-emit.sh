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
# 用 nc 投递一行;无 nc 时用 python 兜底(unix socket 无法用 /dev/tcp)。
if command -v nc >/dev/null 2>&1; then
    printf '%s\n' "$MSG" | nc -U "$SOCK" -w 1 >/dev/null 2>&1 || true
else
    printf '%s\n' "$MSG" | "$(dirname "$0")/.venv/bin/python" -c \
      'import socket,sys,os;s=socket.socket(socket.AF_UNIX);s.connect(os.path.expanduser("~/.cchud/daemon.sock"));s.sendall(sys.stdin.buffer.read());s.close()' \
      >/dev/null 2>&1 || true
fi
exit 0
