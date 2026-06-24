from __future__ import annotations
import asyncio
import json
import os
import sys
from .daemon import Daemon
from .adapters.claude import ClaudeAdapter

HOME = os.path.expanduser("~")
SOCK = os.path.join(HOME, ".cchud", "daemon.sock")
LOG = os.path.join(HOME, ".cchud", "daemon.log")


def merge_claude_settings(settings: dict, emit_path: str) -> dict:
    """纯函数:把 ClaudeAdapter 的 hook_spec 合并进 settings 字典并返回新字典。

    幂等:重复调用不产生重复项。
    statusLine 命令由 emit_path 同目录的 statusline_wrapper 推算。
    """
    s = dict(settings)
    hooks = dict(s.get("hooks", {}))
    spec = ClaudeAdapter().hook_spec()
    for event, args in spec.events.items():
        command = f"{emit_path} {args}"
        entry = {"hooks": [{"type": "command", "command": command}]}
        # 幂等:每个事件只保留指向同一 emit 脚本的唯一项
        hooks[event] = [entry]
    s["hooks"] = hooks
    if spec.statusline_wrapper:
        wrapper = os.path.join(os.path.dirname(emit_path), spec.statusline_wrapper)
        s["statusLine"] = {"type": "command", "command": wrapper, "padding": 0}
    return s


def _install(client: str) -> int:
    """将 adapter hook_spec 写入 ~/.claude/settings.json,先备份原文件。"""
    if client != "claude":
        print(f"未知客户端: {client}", file=sys.stderr)
        return 2
    # emit 脚本与本 cli.py 位于 host 包同级目录
    emit = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "cchud-emit.sh")
    cfg = os.path.join(HOME, ".claude", "settings.json")
    cur = {}
    if os.path.exists(cfg):
        with open(cfg) as f:
            cur = json.load(f)
        with open(cfg + ".cchud-bak", "w") as f:
            json.dump(cur, f, indent=2)
    merged = merge_claude_settings(cur, emit)
    os.makedirs(os.path.dirname(cfg), exist_ok=True)
    with open(cfg, "w") as f:
        json.dump(merged, f, indent=2)
    print(f"已写入 {cfg}(备份 {cfg}.cchud-bak)")
    return 0


def _status() -> int:
    """打印 socket 是否存在、daemon.log 末尾若干行。"""
    print(f"socket: {'存在' if os.path.exists(SOCK) else '缺失'} ({SOCK})")
    if os.path.exists(LOG):
        with open(LOG) as f:
            tail = f.readlines()[-10:]
        print("".join(tail))
    return 0


def _daemon() -> int:
    """读取 CCHUD_ADDR 环境变量,建 Daemon,asyncio.run 常驻。"""
    addr = os.environ.get("CCHUD_ADDR", "")
    d = Daemon(addr, SOCK)

    async def _run():
        await d.start()
        while True:
            await asyncio.sleep(3600)

    asyncio.run(_run())
    return 0


def main(argv: list[str]) -> int:
    """CLI 入口:子命令 daemon / install <client> / status。"""
    if not argv:
        print("usage: cchud {daemon|install <client>|status}", file=sys.stderr)
        return 2
    cmd = argv[0]
    if cmd == "daemon":
        return _daemon()
    if cmd == "install":
        return _install(argv[1] if len(argv) > 1 else "")
    if cmd == "status":
        return _status()
    print(f"未知命令: {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
