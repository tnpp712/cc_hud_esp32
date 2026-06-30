# CC-HUD Daemon 使用指南(第 1 层地基)

> 第 1 层把主机侧从"无状态 hook 脚本"重构为常驻 daemon。本文是 daemon 的安装、运行、协议与排错说明。旧脚本(`cchud-hook.sh` 等)仍保留作回退,但新接入请用 daemon。

## 1. 架构一览

```
各客户端 hooks ──► cchud-emit.sh(瘦客户端,投递 JSON)
                       │ Unix domain socket (~/.cchud/daemon.sock)
                       ▼
              cchud-daemon(常驻 Python 进程)
               ├ AdapterRegistry → ClaudeAdapter   把原生 hook/statusline 规范化为 CcHudEvent
               ├ SessionStore     多会话状态 + TTL(300s)
               ├ QuotaTracker     额度按 resets_at 分窗取最大 + 30s 限频
               ├ Aggregator       聚合态(waiting>tool>thinking>idle)+ 去重
               └ BleLink          常驻 BLE 连接 + 串行发送 + v6/v7 编码
                       │ BLE
                       ▼
                   CC-HUD 设备(固件解析 v1-v7)
```

模块代码在 `host/cchud_daemon/`,每个文件单一职责;纯逻辑模块(codec/quota_tracker/aggregator/session_store)均有单测(`host/tests/`,`pytest` 全绿)。

## 2. 安装

> ⚠️ **Python 必须 ≥ 3.10**(推荐 3.12)。3.9 下 `asyncio.Queue/Event` 在 `BleLink.__init__`(`asyncio.run` 之前)绑死旧事件循环,daemon 一跑 BLE 全程 `got Future attached to a different loop`。重建 venv:`python3.12 -m venv host/.venv`。

```bash
# 1) 依赖(首次):bleak 给 daemon;构建固件还需 platformio
host/.venv/bin/python -m pip install bleak

# 2) 找设备地址(macOS 是 CoreBluetooth UUID),用环境变量注入(别改脚本里的默认地址)
host/.venv/bin/python host/push_quota.py --discover --verbose
echo 'export CCHUD_ADDR=<设备UUID>' >> ~/.zshrc && source ~/.zshrc

# 3) 一键装 hooks(Claude / Codex 各一条;CCHUD_USE_V7=1 配 v7 固件)
CCHUD_ADDR=$CCHUD_ADDR CCHUD_USE_V7=1 host/.venv/bin/python -m cchud_daemon.cli install claude
CCHUD_ADDR=$CCHUD_ADDR CCHUD_USE_V7=1 host/.venv/bin/python -m cchud_daemon.cli install codex
```

`install claude` 会:
- 把 5 个 hook(UserPromptSubmit/PreToolUse/PostToolUse/Stop/Notification)与 statusLine 写入 `~/.claude/settings.json`,**保留该事件下已有的第三方 hook**,仅首次写入 `settings.json.cchud-bak` 备份。
- 渲染 `~/Library/LaunchAgents/io.cchud.daemon.plist`(含 `CCHUD_ADDR`/`CCHUD_USE_V7`)。
- ⚠️ 若你曾用过旧的 `cchud-hook.sh` 直推 hook,install **不会**自动删它(只识别 `cchud-emit.sh` 旧条目)→ 需手动从 `settings.json` 删掉含 `cchud-hook.sh` 的条目,否则新旧抢 BLE。

`install codex` 会:
- 写 `~/.codex/hooks.json`(5 个 `cchud-emit.sh codex` 事件,保留 ping-island/r2c 等第三方 hook)。
- ⚠️ **不要往 `~/.codex/config.toml` 写 `hooks = true`**——新版 Codex 报 `invalid type: boolean, expected struct HooksToml` 起不来。Codex **自动加载** `~/.codex/hooks.json`;**重启 Codex 后按提示 `Trust hooks`** 才生效(信任态由 Codex 自己写进 config.toml 的 `[hooks.state]`)。

> statusLine 指向 `cchud-statusline.sh`,它**保留 ccstatusline 渲染** + 后台把额度 JSON 投递给 daemon,状态栏显示不受影响。本机没装 `bun` 就把里面的 `bunx` 改 `npx`。

## 3. 运行

> ⚠️ **macOS 蓝牙权限**:daemon 必须从**有蓝牙授权的终端**(Terminal/iTerm)启动。launchd 拉起的进程拿不到蓝牙 TCC 授权、又不弹框,会 `BLE is not authorized` 静默失败。所以 macOS 上**别用 launchd 常驻**,用终端跑(可配 tmux / `nohup` 保活)。

```bash
# 常驻运行(从有蓝牙权限的终端;cwd=host 时 -m 能找到包,无需 PYTHONPATH)
cd host && CCHUD_ADDR=$CCHUD_ADDR CCHUD_USE_V7=1 .venv/bin/python -m cchud_daemon.cli daemon
# 首次连接 macOS 弹蓝牙授权 → 点允许

# 查看状态(socket / 日志末尾)
host/.venv/bin/python -m cchud_daemon.cli status
```

| 环境变量 | 含义 |
|----------|------|
| `CCHUD_ADDR` | 设备 BLE 地址 / macOS 外设 UUID(必需) |
| `CCHUD_USE_V7` | `1` 时 quota 走 v7 TLV(`0x0B`),否则 v6(`0x0A`) |

## 4. 协议:v6 与 v7

- **v6(默认)**:固定布局 `0x0A` 帧,与旧 `push_quota.py` 字节一致。
- **v7(`CCHUD_USE_V7=1`)**:TLV 可扩展帧 `0x0B`,`[msg_type][flags][seq][total]` + 连续 `[tag][len][value]`。tag 表见 `docs/superpowers/specs/2026-06-24-cc-hud-roadmap-design.md` 7.4 节。固件未知 tag 跳过,`total!=1` 拒绝(分片留待第 4 层)。旧 v1–v6 路径在固件中原样保留。
- 两种编码表达同一份数据,设备显示等价(已真机验证)。

## 5. 排错

| 现象 | 原因 / 处理 |
|------|------------|
| 发了一帧但设备没更新 | **30s 限频**:`QuotaTracker` 同一窗口 30s 内只推一次。即时验证请等 30s 或临时停 daemon 直接用脚本推。 |
| 设备显示旧额度值 | 多半是限频拦了新帧,显示的是上次推送。等下一次 statusline 触发即可。 |
| ctx/成本/代码行"看不到" | 这些在 STATS 轮播页(stage 2/3),HUD 额度页只显示额度。等轮播或短按翻页。 |
| daemon 终端无日志 | 成功路径不打印,只有 BLE 连接/写入失败才 `warning`。属正常。 |
| 调试时连不上设备 | daemon 常驻独占 BLE 连接(macOS 单连接)。先停 daemon 再用 `push_*.py` 直连。 |
| daemon 起来但不推送 | 检查进程环境 `CCHUD_ADDR` 是否非空(launchd 不继承 shell 环境,靠 plist 的 `EnvironmentVariables`)。 |
| daemon 一跑 BLE 就 `attached to a different loop` | **Python 3.9 的坑**:升到 ≥3.10(`python3.12 -m venv host/.venv` 重建并装 `bleak`)。 |
| daemon `BLE is not authorized` | macOS 蓝牙 TCC 不授权 launchd 进程。改从有蓝牙权限的终端跑 daemon(见 §3)。 |
| BLE 失败看不到日志 | warning 走 **stderr**,不是 `~/.cchud/daemon.log`(那个常空)。前台跑看终端,后台跑看重定向的 stderr。 |
| Codex 启动报 `expected struct HooksToml` | config.toml 里写了 `hooks = true`,删掉。Codex 自动读 `~/.codex/hooks.json`,重启后 `Trust hooks` 即可。 |
| Codex 状态不上报 | 装完 hooks 后**没重启 Codex 或没 Trust hooks**。重启 Codex,按提示信任 hooks。 |

## 6. 已知限制(后续层处理)

- **状态帧仍走 `0x07`**(非 v7),quota 才走 v7;状态 v7 化留待第 2 层。
- v7 `title` 用 UTF-8,固件 `title` 缓冲按字节截断,非 ASCII 标题可能截断;第 2 层引入非 ASCII 客户端名前需处理。
- BLE OTA 慢(实测约 2.7 KB/s);大固件建议用 WiFi OTA(`pio run -e esp32s3_nano_wifi -t upload`)。
