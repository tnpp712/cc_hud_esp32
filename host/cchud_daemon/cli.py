from __future__ import annotations
import asyncio
import json
import os
import sys
from .daemon import Daemon
from .adapters.claude import ClaudeAdapter
from .adapters.codex import CodexAdapter


def render_plist(template: str, *, python: str, home: str,
                 daemon_dir: str, addr: str, use_v7: str) -> str:
    """纯函数:将 plist 模板中的占位符替换为实际值。

    @param template  plist 模板字符串(含 __PYTHON__ 等占位)
    @param python    Python 解释器路径
    @param home      用户 HOME 目录
    @param daemon_dir  host 目录绝对路径(WorkingDirectory)
    @param addr      CCHUD_ADDR 环境变量值
    @param use_v7    CCHUD_USE_V7 环境变量值
    @return 替换完毕的 plist 字符串
    """
    return (template
            .replace("__PYTHON__", python)
            .replace("__HOME__", home)
            .replace("__DAEMON__", daemon_dir)
            .replace("__ADDR__", addr)
            .replace("__USE_V7__", use_v7))

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
        # 保留该 event 下不指向 cchud-emit.sh 的第三方条目,去掉旧的 cchud 条目,再追加新 cchud 条目
        existing = hooks.get(event, [])
        third_party = [
            e for e in existing
            if not any("cchud-emit.sh" in (h.get("command", ""))
                       for h in e.get("hooks", []))
        ]
        hooks[event] = third_party + [entry]
    s["hooks"] = hooks
    if spec.statusline_wrapper:
        wrapper = os.path.join(os.path.dirname(emit_path), spec.statusline_wrapper)
        s["statusLine"] = {"type": "command", "command": wrapper, "padding": 0}
    return s


def merge_codex_hooks(root: dict, emit_path: str) -> dict:
    """纯函数:把 CodexAdapter 的 hook_spec 合并进 ~/.codex/hooks.json 的 dict。

    保留各事件下的第三方 hook(如 ping-island、r2c),仅去重/更新 cchud 条目。幂等。
    Codex 条目结构与 Claude 略不同:matcher 在外层。
    """
    out = dict(root)
    hooks = dict(out.get("hooks", {}))
    spec = CodexAdapter().hook_spec()
    for event, args in spec.events.items():
        command = f"{emit_path} {args}"
        entry = {"matcher": "*",
                 "hooks": [{"type": "command", "command": command}]}
        existing = hooks.get(event, [])
        third_party = [
            e for e in existing
            if not any("cchud-emit.sh" in (h.get("command", ""))
                       for h in e.get("hooks", []))
        ]
        hooks[event] = third_party + [entry]
    out["hooks"] = hooks
    return out


def _backup_once(path: str) -> None:
    """仅当目标存在且备份不存在时,原样复制一份 .cchud-bak 备份。"""
    bak = path + ".cchud-bak"
    if os.path.exists(path) and not os.path.exists(bak):
        with open(path) as f:
            content = f.read()
        with open(bak, "w") as f:
            f.write(content)


def _write_json_config(cfg: str, merged: dict) -> None:
    os.makedirs(os.path.dirname(cfg), exist_ok=True)
    with open(cfg, "w") as f:
        json.dump(merged, f, indent=2)


def _install_plist(host_dir: str) -> None:
    """渲染并写入 launchd plist(通用:一个 daemon 服务所有客户端)。"""
    with open(os.path.join(host_dir, "io.cchud.daemon.plist")) as f:
        tpl = f.read()
    rendered = render_plist(
        tpl, python=sys.executable, home=HOME, daemon_dir=host_dir,
        addr=os.environ.get("CCHUD_ADDR", ""),
        use_v7=os.environ.get("CCHUD_USE_V7", ""))
    launch_agents = os.path.join(HOME, "Library", "LaunchAgents")
    os.makedirs(launch_agents, exist_ok=True)
    plist_dest = os.path.join(launch_agents, "io.cchud.daemon.plist")
    with open(plist_dest, "w") as f:
        f.write(rendered)
    print(f"已写入 plist:{plist_dest}")
    print(f"启动服务请执行:launchctl load {plist_dest}")


def _install(client: str) -> int:
    """安装指定客户端的 hook 配置 + 通用 launchd plist。"""
    host_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    emit = os.path.join(host_dir, "cchud-emit.sh")
    if client == "claude":
        cfg = os.path.join(HOME, ".claude", "settings.json")
        cur = json.load(open(cfg)) if os.path.exists(cfg) else {}
        _backup_once(cfg)
        _write_json_config(cfg, merge_claude_settings(cur, emit))
        print(f"已写入 {cfg}(首次备份 {cfg}.cchud-bak)")
    elif client == "codex":
        cfg = os.path.join(HOME, ".codex", "hooks.json")
        cur = json.load(open(cfg)) if os.path.exists(cfg) else {}
        _backup_once(cfg)
        _write_json_config(cfg, merge_codex_hooks(cur, emit))
        print(f"已写入 {cfg}(首次备份 {cfg}.cchud-bak)")
        print("请确认 ~/.codex/config.toml 含 hooks = true")
    else:
        print(f"未知客户端: {client}(支持 claude / codex)", file=sys.stderr)
        return 2
    _install_plist(host_dir)
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
    use_v7 = os.environ.get("CCHUD_USE_V7", "") == "1"
    from .ble_link import BleLink
    ble = BleLink(addr, use_v7=use_v7)
    d = Daemon(addr, SOCK, ble=ble)

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
