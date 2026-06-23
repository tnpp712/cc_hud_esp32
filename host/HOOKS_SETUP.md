# CC-HUD 主机接入指南(Claude Code / Codex)

把 CC-HUD 硬件接到 Claude Code 与 Codex:实时显示**工作状态**(思考/工具/等待/空闲)
和**额度**(5h/7d 用量、会话开销),全程走 BLE,不依赖 WiFi。

本文同时面向**人**和**AI IDE**:第 5 节给出可直接粘贴给 AI 的一键指令。

---

## 1. 设备地址速查(macOS CoreBluetooth UUID)

> ⚠️ 这些 UUID 只在**对应那台 Mac** 上有效:macOS 不暴露蓝牙真实 MAC,CoreBluetooth
> 给「每台主机 × 每个设备」各自生成 UUID。换 Mac、重装系统、清蓝牙缓存都可能让它变。
> 变了就用 `push_quota.py --discover` 重新发现(设备名 `CC-HUD` 是固件广播的,跨机器不变)。

| 用途 | 地址 (UUID) | 设备名 | 说明 |
|---|---|---|---|
| **当前设备(在用)** | `EC875770-C5EA-539F-2A65-EAF348861E29` | `CC-HUD` | 现在连接的设备,已写入全部脚本默认值 |
| 旧设备 | `0A12EA0B-ECB0-6020-64E9-8C72447DEB51` | `CC-HUD` | 最早连接过的那块 HUD |
| 原作者仓库内置 | `03265204-7126-B6FE-6F17-6E9621CEA97F` | `CC-HUD` | 原作者(firshme)Mac 的 UUID,**在别的 Mac 上无效** |

**UUID 写在这 4 个脚本里**(默认值,可被环境变量 `CCHUD_ADDR` 覆盖):
`cchud-hook.sh`、`cchud-quota-push.sh`、`cchud-update.sh`、`cchud-codex-notify.sh`。

---

## 2. 组成与文件清单

| 文件 | 作用 | 触发方 |
|---|---|---|
| `cchud-hook.sh` | 把 Claude Code 的应用状态(思考/工具/空闲/等待)推到 HUD;多会话聚合取最高优先级;BLE 串行锁 | Claude Code 5 个 hook 事件 |
| `cchud-statusline.sh` | statusLine 包装器:原样渲染 `ccstatusline` + 后台推额度,不阻塞状态栏 | Claude Code `statusLine` |
| `cchud-quota-push.sh` | 从 statusline JSON 取官方 `rate_limits` 百分比(5h/7d)+ 会话开销 + 模型名标题,跨会话取最大值合并 | 被 `cchud-statusline.sh` 调用 |
| `cchud-update.sh` | 限频(30s)、后台、串行化的额度 BLE 推送(传输层) | 被 `cchud-quota-push.sh` 调用 |
| `cchud-codex-dispatch.sh` | Codex `notify` 分发器:保留原有 notify(如 computer-use)+ 追加 CC-HUD | Codex `notify` |
| `cchud-codex-notify.sh` | Codex → CC-HUD 适配:回合结束推绿色 done-pulse | 被 dispatch 调用 |
| `push_state.py` / `push_quota.py` | 底层 BLE 写入(bleak);`push_quota.py --discover` 用于发现设备 | 被上面脚本调用 / 手动 |

状态映射:`UserPromptSubmit→thinking`、`PreToolUse→tool`、`PostToolUse→thinking`、
`Stop→idle`、`Notification→waiting`(良性提示自动降级为 idle)。

---

## 3. 新电脑首次配置

> 约定:`CCHUD_DIR` = 本仓库的 `host` 目录绝对路径(例:`/Users/<you>/github/cc_hud_esp32/host`)。
> 下面命令把它先设成变量,后续复用。

### 3.1 依赖

```bash
# 必需:jq(解析 hook/statusline JSON)、Python venv + bleak(BLE)
brew install jq            # 没装 Homebrew 就用系统包管理器
command -v uv >/dev/null || curl -LsSf https://astral.sh/uv/install.sh | sh

CCHUD_DIR="$(cd "$(dirname "$0")" 2>/dev/null && pwd)"   # 或手动填绝对路径
cd "$CCHUD_DIR"
uv venv .venv
uv pip install --python .venv/bin/python -r requirements.txt
.venv/bin/python -c "from bleak import BleakClient; print('bleak OK')"
# 额度功能还需要 bunx(随 bun 安装),且你已在用 ccstatusline;只要状态灯可不装
```

### 3.2 发现设备 UUID

给 HUD 通电(USB-C 或电池+自锁开关,屏幕亮、在广播),然后:

```bash
cd "$CCHUD_DIR"
.venv/bin/python push_quota.py --discover --verbose --timeout 8
# 输出形如:
# ADDRESS                                   NAME     RSSI
# XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX      CC-HUD   -42 dBm
```

记下 `CC-HUD` 那行的地址,设为 `NEW_UUID`。

### 3.3 写入 UUID(一条命令改全部 4 个脚本)

```bash
cd "$CCHUD_DIR"
NEW_UUID="XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"   # ← 换成 3.2 发现的
# macOS 的 sed 需要 -i ''(GNU/Linux 用 sed -i)
sed -i '' -E "s/CCHUD_ADDR:-[0-9A-Fa-f-]+/CCHUD_ADDR:-$NEW_UUID/" \
  cchud-hook.sh cchud-quota-push.sh cchud-update.sh cchud-codex-notify.sh
grep -l "$NEW_UUID" cchud-hook.sh cchud-quota-push.sh cchud-update.sh cchud-codex-notify.sh
```

### 3.4 Claude Code hooks(合并进 `~/.claude/settings.json`)

把下面 5 项**合并**进现有 `hooks`(不要整体覆盖,保留你已有的 hook)。
把 `<CCHUD_DIR>` 替换为绝对路径。

```json
{
  "hooks": {
    "UserPromptSubmit": [{ "hooks": [{ "type": "command", "command": "<CCHUD_DIR>/cchud-hook.sh thinking" }] }],
    "PreToolUse":  [{ "matcher": "*", "hooks": [{ "type": "command", "command": "<CCHUD_DIR>/cchud-hook.sh tool" }] }],
    "PostToolUse": [{ "matcher": "*", "hooks": [{ "type": "command", "command": "<CCHUD_DIR>/cchud-hook.sh thinking" }] }],
    "Stop":         [{ "hooks": [{ "type": "command", "command": "<CCHUD_DIR>/cchud-hook.sh idle" }] }],
    "Notification": [{ "hooks": [{ "type": "command", "command": "<CCHUD_DIR>/cchud-hook.sh waiting" }] }]
  },
  "statusLine": { "type": "command", "command": "<CCHUD_DIR>/cchud-statusline.sh", "padding": 0 }
}
```

- `statusLine` 是**额度推送**入口(可选)。`cchud-statusline.sh` 内部会调 `bunx -y ccstatusline@latest`
  渲染你原来的状态栏 —— 若你用别的状态栏工具,改这一行包装即可;只要状态灯不配 `statusLine` 也行。
- hooks **会话启动时加载**,改完要**新开会话**才生效。

### 3.5 Codex notify(`~/.codex/config.toml`)

**情况 A:你没有其它 notify**(最简单)——直接指向 CC-HUD 适配器:

```toml
notify = ["<CCHUD_DIR>/cchud-codex-notify.sh"]
```

**情况 B:你已有 notify**(例如 computer-use,想保留)——用分发器,并把它内部的
`ORIG` 改成你原来的 notify 程序绝对路径:

```toml
notify = ["<CCHUD_DIR>/cchud-codex-dispatch.sh"]
```

```bash
# 编辑 cchud-codex-dispatch.sh,把 ORIG 改成本机的 computer-use(或你的)notify:
#   ORIG="/Users/<you>/.codex/computer-use/.../SkyComputerUseClient"
```

Codex 同样需**新开会话**生效。

### 3.6 验证(不依赖真机也能验证接线)

```bash
cd "$CCHUD_DIR"
# 真机直推一发状态,屏幕应闪「工具/Bash」:
.venv/bin/python push_state.py --address "$NEW_UUID" --state tool --detail Bash --timeout 8
# 真机直推额度,屏幕应显示 5h/7d 条:
CCHUD_MODE=sub ./cchud-update.sh 8 100 4 100 0 0
# 退出码 0 + 屏幕响应 = 成功。然后新开 Claude/Codex 会话,实跑观察。
```

---

## 4. 旧电脑切换 UUID(换了设备 / UUID 变了)

只需「重新发现 + 一条 sed」:

```bash
cd "$CCHUD_DIR"
.venv/bin/python push_quota.py --discover --verbose --timeout 8   # 拿到新 UUID
NEW_UUID="XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"
sed -i '' -E "s/CCHUD_ADDR:-[0-9A-Fa-f-]+/CCHUD_ADDR:-$NEW_UUID/" \
  cchud-hook.sh cchud-quota-push.sh cchud-update.sh cchud-codex-notify.sh
```

无需重配 hooks(脚本路径没变)。`statusLine`/hooks 是热的额度部分立即生效;
状态 hook 因缓存在会话内,**新开会话**后用新 UUID。

> 临时只想换一次、不改脚本:`export CCHUD_ADDR=新UUID`(脚本默认值会让位给环境变量)。

---

## 5. 给 AI IDE 的一键指令(直接粘贴)

> 把下面整段发给 AI IDE(Claude Code / Cursor / Codex 等),并按提示补上你的实际路径。

```
请帮我把本仓库 host/ 目录下的 CC-HUD hooks 配置到 Claude Code 和 Codex,按 host/HOOKS_SETUP.md 执行:
1. 在 host/ 下用 uv 建 .venv 并安装 requirements.txt(bleak);确认 jq 已安装。
2. 运行 `.venv/bin/python push_quota.py --discover --verbose --timeout 8` 发现我的设备 UUID
   (设备名 CC-HUD);把发现到的 UUID 用一条 sed 写入 cchud-hook.sh / cchud-quota-push.sh /
   cchud-update.sh / cchud-codex-notify.sh 的 CCHUD_ADDR 默认值。
3. 把 HOOKS_SETUP.md 第 3.4 节的 5 个 hook + statusLine **合并**进 ~/.claude/settings.json
   (保留我已有的 hook,不要覆盖;命令里的路径用 host 目录绝对路径)。用 jq/python 校验 JSON 合法。
4. 按 3.5 节配置 ~/.codex/config.toml 的 notify(我没有其它 notify 就用情况 A 直连 cchud-codex-notify.sh)。
   先备份 settings.json 和 config.toml。
5. 按 3.6 节做真机验证(push_state.py 推一发 tool、cchud-update.sh 推一发额度),报告退出码和屏幕反应。
注意:hooks 改完需新开会话才生效;UUID 是 macOS CoreBluetooth UUID、本机专属。
```

---

## 6. 故障排查

| 现象 | 原因 / 处理 |
|---|---|
| `Device ... not found` | 设备没通电/没广播,或 UUID 不对(换机器/重装后会变)→ 重新 `--discover` 改 UUID;`--timeout 20` 再试 |
| `--discover` 扫不到 | 蓝牙没开 / 终端无蓝牙权限(系统设置→隐私与安全性→蓝牙)/ 设备未上电 |
| 状态灯一直 thinking 停不下 | 已修(固件:quota 推送不再误触发 thinking)。若仍有,确认设备已刷最新固件 |
| 额度数字与官方面板对不上 | 额度取自 statusline JSON 的官方 `rate_limits` 百分比;多会话取最大值合并,与面板一致(允许刷新延迟) |
| 灯环不亮但屏幕正常 | 灯环走 MT3608(自锁开关之后);纯 USB 没拨开关时灯环无电,拨开关走电池链路 |
| 日志 | 状态:`/tmp/cchud-state.log`;额度:`/tmp/cchud-push.log`;Codex:`/tmp/cchud-codex.log` |

环境变量覆盖:`CCHUD_ADDR`(地址)、`CCHUD_RATE_LIMIT`(额度限频秒,默认 30)、
`CCHUD_5H_LIMIT`/`CCHUD_7D_LIMIT`(已弃用,现按官方百分比)、`CCHUD_TITLE`(覆盖标题)。
