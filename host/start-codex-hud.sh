#!/usr/bin/env bash
# 从当前仓库启动；可通过 host/.env 设置设备地址和天气城市。
set -euo pipefail
cd "$(dirname "$0")"
if [ -f .env ]; then
    set -a
    source .env
    set +a
fi
exec .venv/bin/python -m cchud_daemon.cli daemon
