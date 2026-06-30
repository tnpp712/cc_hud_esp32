# cc_hud · AI 状态灯

放在桌上的一个小硬件 HUD：一块 1.54″ 彩屏显示 **Claude Code / Codex 的额度、花费、上下文、状态**，外圈 24 颗灯随 AI "在干嘛"红黄绿变化。ESP32-S3 驱动，电脑通过蓝牙/WiFi 把状态推给它。

> 本仓库是 [`uk0/cc_hud_esp32`](https://github.com/uk0/cc_hud_esp32) 的私有 fork，在原版"BLE 推额度 + LCD HUD"基础上做了大量产品级增强（见下）。英文原版说明见 [`README.en.md`](README.en.md)。

<p align="center">
  <img src="images/fork-hud.jpg" alt="HUD 显示额度" width="44%" />
  &nbsp;&nbsp;
  <img src="images/fork-ring.jpg" alt="设备 + 24 灯环" width="44%" />
</p>

<p align="center"><em>左：屏幕实时显示 5H/7D 额度 + 重置倒计时 + 上下文用量。右：屏 + 24 颗 WS2812B 状态灯环。</em></p>

---

## 🔱 本 fork 新增功能

**显示 / UI**
- **4 屏轮播状态机**：额度页 / 统计页（成本·时长·增删行·5H 重置倒计时）/ 工具页（大图标 + N 会话·M 忙）/ 时钟页；空闲自动轮播、干活锁工具页、防抖
- **燃烧率耗尽预测**：按当前用速预测"X 窗口会在重置前耗尽" → footer 红字预警
- 更大字号 HUD + CJK 中文天气

**24 灯环状态灯（WS2812B，`firmware/src/led_ring.*`）**
- 空闲 = 绿色**额度表**（点亮颗数 = 额度%、分级变色）
- 工作 = 黄色**彗星** · 等权限 = 红色**整环闪** · 低电 = 橙色慢闪
- **Codex 完成绿脉冲**：AI 答完一轮闪三下

**无线 / OTA / 网页（`firmware/src/wifi_ota.*`）**
- **WiFi OTA**：`cc-hud.local` 局域网秒级刷机（BLE OTA 保留作救砖）
- **网页面板** `http://cc-hud.local/`：实时额度/成本/状态/电量，**亮度滑块（存 NVS、免重刷）**、翻页/调暗遥控、固件版本/运行时长/内存
- **NTP 自动校时**：WiFi 在线自动授时，断电后免手动校准

**智能 / 省电**
- 夜间(23:00–07:00)+ 长空闲自动调暗、空闲 10 分钟熄灯环
- 修复 USB-CDC-on-boot 导致的"插电脑屏黑"（`displayInit` 前移）

**多 AI 工具支持**
- **Codex CLI**：`~/.codex/config.toml` 的 `notify` → 适配器，答完触发灯环绿脉冲；链式调度器保留已有 notify（如 computer-use）
- 任意工具调 `host/push_state.py` 即可推状态；多工具/多会话**统一聚合**（有人忙就显示忙）

**可选硬件（接上即生效，不接不误报）**
- 电池电量监测（2×2.2kΩ 分压 → 丝印 A0）+ 低电告警
- 实体按钮（→ 丝印 A1）短按翻页、长按调暗

---

## 硬件一览

| 部件 | 选型 |
|---|---|
| 主板 | ESP32-S3-Nano（N8R8/N16R8，兼容 Arduino Nano ESP32 引脚） |
| 屏 | 1.54″ 240×240 IPS ST7789（4 线 SPI，8pin） |
| 灯环 | WS2812B **24 位**单圈圆环（内径 ≥56mm）+ 470Ω 电阻 + 470µF 电容 |
| 电源 | 3.7V 锂电（带保护板）+ TP4056 充电 + MT3608 升压 5V + 自锁开关 |
| 可选 | 2×2.2kΩ（电量）、自复位按钮、亚克力盒 |

> ⚠ 丝印坑：这块板低位 GPIO 标 **A0–A7**，灯环 DIN 接 **D2(=GPIO5)** 不是 "D5"。完整对照见制作指南。

---

## 快速上手

**完整图文制作指南** → **[`docs/build-guide.html`](docs/build-guide.html)**（康奈尔笔记 + 分镜，浏览器直接打开：引脚总表 / 焊接 / 接线 / 调 5V / 刷机 / 排障 / 多工具）。

最短路径：

1. **焊屏**：屏 8 线接主板（VCC→3V3，**别接 5V**）
2. **焊电源**：电池→TP4056→开关→MT3608，**先单独调好 5.0V** 再接主板
3. **焊灯环**：DIN 经 470Ω → 主板 D2(GPIO5)；VCC → MT3608 IN+（开关后 3.7V）；470µF 跨 VCC/GND
4. **刷固件**：USB 首刷 → 之后 WiFi OTA 秒刷
   ```bash
   cd firmware
   pio run -e esp32s3_nano -t upload            # USB 首刷
   pio run -e esp32s3_nano_wifi -t upload       # 之后 WiFi 秒刷
   ```
5. **配电脑**（目录 `host/`，下面命令都在这个目录里跑）

   电脑在这套系统里是"中间人"：读 AI 的状态/额度 → 通过蓝牙推给盒子。核心是一个常驻后台进程 **daemon**，它**独占一条蓝牙连接**，把多个 AI 会话的状态聚合后推给设备。按 ①→⑤ 配一遍即可。

   **① 建 Python 环境**
   ```bash
   python3.12 -m venv .venv && .venv/bin/pip install bleak
   ```
   - `venv` 是隔离的 Python 环境，依赖只装进 `host/.venv/`，不污染系统 Python；`bleak` 是跟蓝牙设备通信的库。
   - **为什么必须 Python ≥ 3.10**：daemon 启动时会建一个"蓝牙发送队列"，Python 3.9 会把这个队列错绑到旧的事件循环上，导致 daemon 一跑就满屏 `attached to a different loop`、蓝牙完全发不出去；3.10 起改了这个行为才正常。没有 3.12 就 `brew install python@3.12`。

   **② 找设备地址，存成环境变量**
   ```bash
   .venv/bin/python push_quota.py --discover          # 列出附近设备，记下你盒子的 UUID
   echo 'export CCHUD_ADDR=<上一步的UUID>' >> ~/.zshrc && source ~/.zshrc
   ```
   - macOS 上蓝牙设备没有固定 MAC，只有系统分配的一串 UUID，所有推送都要靠它指明"发给哪个盒子"。
   - **为什么写进 `~/.zshrc`、不改脚本**：地址放进环境变量后，仓库脚本保持原样——`git pull` 不冲突，也不会把你的设备地址提交进代码。之后每个命令都用 `$CCHUD_ADDR` 引用它。

   **③ 给盒子配 WiFi**（一次性，经蓝牙写入）
   ```bash
   .venv/bin/python push_wifi.py --address $CCHUD_ADDR --ssid 'WiFi名' --password '密码'
   ```
   - **为什么要联网**：盒子联网后能自动 NTP 校时、用 WiFi 秒级 OTA 刷固件、开网页面板 `http://cc-hud.local/`。不联网也能用蓝牙，但这些功能没了。

   **④ 装 hooks**（让 AI 自动上报状态）
   ```bash
   CCHUD_ADDR=$CCHUD_ADDR CCHUD_USE_V7=1 .venv/bin/python -m cchud_daemon.cli install claude
   CCHUD_ADDR=$CCHUD_ADDR CCHUD_USE_V7=1 .venv/bin/python -m cchud_daemon.cli install codex
   ```
   - **hook** = AI 工具在关键时刻（开始思考、调工具、等你确认、结束）自动运行的小脚本，这里就是把状态投给 daemon。
   - 两条命令分别给 Claude Code 和 Codex 各装一套，会自动写进它们各自的配置并**保留你已有的第三方 hook**；`CCHUD_USE_V7=1` 表示用新版 v7 协议（配最新固件）。

   **⑤ 起常驻 daemon**
   ```bash
   CCHUD_ADDR=$CCHUD_ADDR CCHUD_USE_V7=1 .venv/bin/python -m cchud_daemon.cli daemon
   ```
   - 保持这个终端窗口开着，它就一直把状态推给盒子；终端没刷出错误 = 正常。
   - **为什么必须在普通终端跑、不能用 launchd 开机自启**：macOS 的蓝牙权限按"谁启动了这个进程"授权——普通终端有授权，而 launchd 拉起的后台进程拿不到、还不弹授权框，会静默报 `BLE is not authorized`。想保活可以用 tmux 或 `nohup`。

   **Codex 额外两点**
   - 装完 hooks 要**重启 Codex**，启动时会弹"是否信任 hooks"，选 **Trust all and continue**——新版 Codex 不信任就不跑 hook。
   - ⚠️ **千万别往 `~/.codex/config.toml` 写 `hooks = true`**：新版 Codex 会报 `expected struct HooksToml` 直接起不来。`~/.codex/hooks.json` 是自动加载的，不需要这行。

   > 状态栏渲染用 `ccstatusline`；本机没装 `bun` 的话，把 `cchud-statusline.sh` 里的 `bunx` 改成 `npx`。完整步骤 / 排错见 [`host/DAEMON.md`](host/DAEMON.md)。

---

## 多 AI 工具

| 工具 | 显示 | 接法 |
|---|---|---|
| **Claude Code** | 完整：额度 + 上下文 + 成本 + 4 状态灯 + 多会话 | `cchud_daemon.cli install claude`（写 `hooks` + `statusLine`，保留第三方） |
| **Codex CLI** | 状态 + 多会话（思考/工具/等待/空闲） | `cchud_daemon.cli install codex`（写 `~/.codex/hooks.json`，重启 Codex 后 **Trust hooks**；**勿写 `hooks=true`**） |
| 其它 | 任意脚本调 `push_state.py` 推状态/灯色 | 通用 CLI |

---

## 目录结构

```
firmware/        ESP32-S3 固件（PlatformIO · LVGL 9 · NimBLE）
  src/
    main.cpp           主循环 + 状态聚合 + NTP + 省电
    led_ring.*         24 灯环状态灯
    wifi_ota.*         WiFi OTA + 网页面板
    lvgl_ui.*          4 屏 UI
    display.* ble_server.* persistence.* battery.* button.*
host/            电脑端 Python + shell
  push_wifi.py / push_state.py / push_quota.py / push_idle.py / ota.py
  cchud-hook.sh / cchud-update.sh / cchud-idle.sh   (Claude Code)
  cchud-codex-notify.sh / cchud-codex-dispatch.sh   (Codex)
docs/build-guide.html  完整图文制作指南
README.en.md           英文原版说明（upstream）
```

---

*基于 [uk0/cc_hud_esp32](https://github.com/uk0/cc_hud_esp32)。本 fork 增强见上。*
