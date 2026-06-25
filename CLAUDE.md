# CC-HUD ESP32 — CLAUDE.md

减少常见编码错误的行为准则 + 本项目特定约定。

**权衡:** 准则偏向谨慎而非速度;琐碎任务用判断力。

## 一、通用行为准则

### 1. 想清楚再写

不臆断、不藏困惑、亮明权衡。动手前:显式陈述假设,不确定就问;有多种理解时摆出来,别默默选一个;有更简单的做法就说,该 push back 就 push back;不清楚就停下,先指出困惑点再问。

### 2. 简洁优先

解决问题的最少代码,不做投机。不加没要求的功能 / 抽象 / "灵活性";不为不可能的场景写错误处理;200 行能压成 50 就重写。自问:"资深工程师会觉得这过度设计吗?"

### 3. 外科手术式改动

只动必须动的,只清自己的烂摊子。改既有代码时:不顺手"改进"相邻代码 / 注释 / 格式;不重构没坏的东西;匹配既有风格;发现无关死代码只提及、不删。只删自己改动产生的孤儿(import / 变量)。检验:每改一行都能直接追溯到需求。

### 4. 目标驱动执行

定义可验证的成功标准,循环到验证通过。把任务转成可验证目标("修 bug"→"写复现测试再让它过")。多步任务先列简短计划(步骤 → 验证点)。强标准能独立循环,弱标准("能用就行")需反复澄清。

## 二、架构

- 主机侧:常驻 daemon `host/cchud_daemon/`(adapter 层 → SessionStore/QuotaTracker/Aggregator → BleLink),取代旧无状态 hook 脚本。
- 协议:BLE v7 TLV(msg_type `0x0B`),tag 表见 `docs/superpowers/specs/2026-06-24-cc-hud-roadmap-design.md` 7.4 节;旧 v1–v6 路径在固件中保留。
- 固件:ESP32-S3 + LVGL,`firmware/src/`。协议解析剥离为纯函数 `v7_tlv.cpp`(不依赖 NimBLE/Arduino,可 native 单测)。

## 三、测试与构建

- host 单测:`cd host && .venv/bin/python -m pytest -q`
- 固件 native 单测:`cd firmware && pio test -e native`(纯逻辑,无需真机)
- 固件编译:`cd firmware && pio run -e esp32s3_nano`
- 新增协议 / 逻辑务必抽成纯函数走单测,把真机依赖压到最小。

## 四、daemon 运行 / 调试

- 起 daemon:`CCHUD_ADDR=<UUID> CCHUD_USE_V7=1 [CCHUD_WEATHER_CITY=杭州] host/.venv/bin/python -m cchud_daemon.cli daemon`
- 本机设备地址:`EC875770-C5EA-539F-2A65-EAF348861E29`
- 安装 hook:`cchud install claude|codex`(保留第三方 hook + 备份)
- **调试需独占 BLE:先 `pkill -f "cchud_daemon.cli daemon"`**(macOS 一次只允许一个 BLE 连接)。
- 设备发现:`host/.venv/bin/python host/push_quota.py --discover`

## 五、踩过的坑(本项目实测,务必避免)

每条都是上面准则的具体反面教材:

- **冒烟测试必清残留**(对应准则 3):手动 `cchud-emit.sh` 推的状态会在 SessionStore 留会话(TTL 300s),卡住设备(红环 / thinking),极易误判成 bug。冒烟后立即推对应 `Stop` 清除;调试加的临时 emit-debug 日志 / 代码用完立刻移除。
- **先抓真实 hook 样本,再写 adapter**(对应准则 1):事件名 / 字段 / 权限模式只能靠真实 stdin 确认(如 Codex 用 `request_user_input` 工具问用户、`permission_mode=bypassPermissions` 表示自动批准不需提醒),别照调研或文档推断直接写——一帧真实样本就能推翻整份推断。
- **重构旧脚本要清点全部职责**(对应准则 1):daemon 化时漏迁了 `push_idle.py` 的时间 / 天气推送(旁路职责),导致设备时钟错、天气消失。主路径迁干净了,旁路功能会静默失效,真机才暴露。迁移前列全主路径 + 旁路职责。
- **状态机要自愈**:纯事件驱动会因会话异常结束(关窗口 / 崩溃 / 断网,无 `Stop`)卡在最后态;daemon 用周期 tick 重聚合兜底(`_tick_loop`,TTL 过期自动回 idle)。
- **跨语言文本用 UTF-8**:中文天气 / 标题不能 ascii 编码(会乱码),按字节安全截断别切断 CJK 字符。
- **真机验证慢**:BLE OTA ~9 分钟(约 2.7 KB/s),能联网时优先 WiFi OTA(`pio run -e esp32s3_nano_wifi -t upload`)。
- **固件 clang LSP 误报**:编辑器报 `Arduino.h`/`uint8_t`/`unity.h` not found 是缺 PlatformIO include 路径的误报,以 `pio test`/`pio run` 实际结果为准。
- **commit message 避免双引号**:`git commit -m "…含中文双引号…"` 会破坏 shell 命令,用单引号。
