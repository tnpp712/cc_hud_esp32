# Codex HUD · 桌面状态屏

把 Codex 的运行状态、额度、上下文和会话统计显示在 ESP32-S3 桌面设备上。主机通过 BLE 常驻连接，固件驱动 240×240 彩屏和 24 颗灯环。

基于 [uk0/cc_hud_esp32](https://github.com/uk0/cc_hud_esp32) 的 fork，2026-10-23 将默认客户端、图标、思考动画、文字和配色改为 Codex。按用户要求移除 5H 额度及相关预警。原有 BLE UUID、NVS 命名空间、WiFi 配置与 `cc-hud.local` 地址保持兼容；旧协议中的 5H 字段仅作为兼容占位，不参与显示、预警、灯环或宠物状态。

## 功能

| 功能 | Codex 版本行为 |
|---|---|
| 工作页 | 项目名、会话标题与固定短编号、模型、思考强度、每会话上下文已用比例、待回答数量、当前阶段、工具、结构化计划步骤、本轮耗时、会话计数 |
| 多会话 | 人工等待优先，其次工具执行、思考和空闲；同类状态按最近活动排序，最多浏览 8 个会话详情 |
| 时钟页 | 全部空闲 3 分钟后显示时钟、天气和字符宠物；新任务开始返回工作页 |
| 灯环 | 工作彗星、等待红闪、完成绿脉冲、低电提示；空闲时亮灯颗数表示 7D 剩余额度 |
| 按钮 | 短按依次切换会话，最后进入时钟，手动选择保持 10 秒；长按约 0.8 秒调暗／恢复 |
| 网页面板 | 保留 7D 精确用量、重置信息、上下文、费用与行数；翻页、调暗和灯环亮度控制 |
| 升级 | WiFi OTA 与 BLE OTA；支持版本和已接收工作信息的只读回读 |
| 兼容 | 旧 v1–v6 与 v7 保留；工作页元数据使用 v7 扩展，旧协议状态仍能显示 |

5H 及其预警已移除。屏幕不再轮播额度和重置页；额度精确值在网页查询，余量用灯环表达。未知额度时空闲灯环熄灭，避免把未知显示为满额。

### 数据含义

- 大字显示本机 Codex 保存的会话标题，小字显示项目名及固定短编号；同项目、同名会话也可通过编号区分。标题随本机索引改名更新，检查间隔约 30 秒；缺失时显示“未命名会话”。编号由会话 ID 派生并在 daemon 状态目录持久化，碰撞时给新会话加长，排序与正常重启不改变编号。右上角的 `2/3` 只表示浏览位置。
- 长标题横向滚动；BLE 每帧上限为 244 字节，标题使用原有字段之后的剩余空间，最多 144 字节，按完整 UTF-8 字符截断。主机状态保留完整标题。
- 额度与上下文来自本机 `CODEX_HOME/sessions` 下的会话日志，默认 `~/.codex/sessions` 。通过只读会话索引和 hook 登记发现活跃日志，支持几天前创建、今天仍在工作的会话；索引不可用时降级扫描近两天目录。日志增量读取。
- 只采集 `window_minutes=10080` 的 7D 额度，不再采集或展示 5H。无论该窗口出现在 `primary` 还是 `secondary` 都能识别。
- 工作页上下文使用该会话最近一次请求的 token 数与其上下文窗口计算，表示已用比例；思考强度取真实会话元数据，缺失时显示 `--`。
- 异步问题确认入队后显示“待回答 N”，同时保留实际工作阶段。按问题 ID 接收回答，全部回答后清除提醒；工具返回“已接受”不会当作已回答。普通用户消息开启新一轮对话时，上一轮提醒退出；同轮追问、环境更新和带 ID 的部分回答不会清掉其余问题。中断、会话关闭和提交失败也会清除对应提醒，普通工具完成不会误清除。
- 行数在网页展示，只统计日志中成功的 `FileChange`，不包括 shell 或外部编辑器写入，也不是当前 Git 净差异。
- 项目和模型来自真实 hook 与会话元数据；步骤只来自 `PostToolUse` 中的结构化 `update_plan` 信息。没有计划数据时隐藏步骤行。
- 使用 `tool_use_id` 配对并行工具，自动审批不清除正在运行的工具；中断或结束后的迟到工具结果不重新点亮工作状态。内部 `codex-auto-review` 会话不计入用户工作会话。
- 订阅日志没有实际美元账单；费用显示“未提供”。缺失的额度、上下文、行数显示未知，不冒充零，也不沿用 Claude 数据。
- 旧数据按原有时效提示显示过期。费用与其它指标仍可通过旧通用协议推送。

## 本机启动

本机原有 `io.cchud.daemon` 已迁移到当前仓库，保留登录启动与自动保活，并已实测 BLE 连接成功。服务运行时不要重复启动终端 daemon。以下命令用于首次配置或手动调试。

依赖 Python 3.13。首次配置：

```bash
uv venv host/.venv --python 3.13
uv pip install --python host/.venv/bin/python -r host/requirements.txt pytest pytest-asyncio
host/.venv/bin/python host/push_quota.py --discover
```

将设备地址保存到不提交 Git 的 `host/.env` ：

```bash
CCHUD_ADDR=你的设备UUID
CCHUD_USE_V7=1
# 可选：CCHUD_WEATHER_CITY=杭州
```

安装状态 hooks：

```bash
cd host
.venv/bin/python -m cchud_daemon.cli install codex
```

安装会保留第三方 hooks。Codex 要求用户信任新 hook 定义：重启后按提示确认，或在 CLI 中用 `/hooks` 审阅。不要设置 `hooks = true`，也不要绕过信任校验。行为依据 [OpenAI 官方 Hooks 文档](https://developers.openai.com/codex/hooks) 。

从有蓝牙权限的终端启动：

```bash
./host/start-codex-hud.sh
```

需要后台运行时：

```bash
mkdir -p .local-backup
nohup ./host/start-codex-hud.sh > .local-backup/daemon.log 2>&1 &
```

同一设备只运行一个 daemon。启动会申请进程锁；调试或 OTA 前，先停止明确属于该项目的 daemon。macOS 的 launchd 蓝牙授权与交互终端可能不同，因此通用安装器不自动创建或启动服务；本次迁移沿用并验证了本机已有的后台服务。

## 固件构建与升级

```bash
cd firmware
pio test -e native
pio run -e esp32s3_nano
# USB 首刷
pio run -e esp32s3_nano -t upload
# 已配 WiFi 时
pio run -e esp32s3_nano_wifi -t upload
```

BLE OTA：

```bash
host/.venv/bin/python host/ota.py \
  --address 你的设备UUID \
  --firmware firmware/.pio/build/esp32s3_nano/firmware.bin \
  --chunk 244 --verbose
```

OTA 必须收到设备 `OK` 才记为传输成功；重启后读取 `12345678-aaaa-bbbb-cccc-1234567890a3` 固件版本，再进行真机验收。保留原始固件用于回滚，勿以“全部字节已发送”替代升级成功。

## 开发与验证

```bash
cd host
.venv/bin/python -m pytest -q
```

图标为代码生成的终端风格资源：

```bash
uv pip install --python host/.venv/bin/python pillow
host/.venv/bin/python firmware/tools/generate_codex_assets.py
```

中文字体的可复现 `lv_font_conv` 参数保存在 `firmware/src/font_cn_20.c` 文件头。小字项目行与步骤的完整中文子集通过 `firmware/tools/generate_session_font.py` 生成，使用压缩字库并启用 LVGL 解码。新增屏幕文字时必须同步字体子集。

| 目录 | 内容 |
|---|---|
| `host/cchud_daemon/` | hooks、会话聚合、Codex 日志用量采集、BLE 连接 |
| `firmware/src/` | LVGL 界面、协议、灯环、电池、按钮、WiFi 和 OTA |
| `firmware/tools/` | 图标生成脚本与预览 |
| `docs/build-guide.html` | 原有硬件制作与接线指南；其中 Claude 安装说明属于历史版本 |
| `AGENTS.md` | 当前项目开发约定 |

硬件仍为 ESP32-S3-Nano、1.54 英寸 ST7789、24 位 WS2812B 灯环；可选电池监测和按钮。接线与电源要求未改变。

## 实时状态与界面验证

```bash
cd host
.venv/bin/python -m cchud_daemon.cli status
```

输出包括主机会话信息与 `device_work`，后者是设备实际已接收的首个会话快照，不能等同用户手动选中的页面。工作快照按代次完整替换，尚未发送的快照只保留最新一份。

可以直接编译实际 LVGL 代码进行离屏渲染，不用另做近似网页：

```bash
cmake -S firmware/tools/ui-preview -B /tmp/cchud-ui-preview -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/cchud-ui-preview -j 8
cd /tmp/cchud-ui-preview
./hud-preview
```

输出工作、等待、多会话与时钟的 PPM 图像，使用示例数据验证布局。主字体包含约 3758 个常用中文字形，罕用字需扩充字库。
