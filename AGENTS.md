# Codex HUD 项目约定

## 架构与边界

- 主机：`host/cchud_daemon/` ，事件适配、会话聚合、额度采集与独占 BLE 连接。
- 固件：`firmware/src/` ，ESP32-S3、ST7789 240×240、LVGL、24 灯环。
- 保留 BLE UUID、NVS 命名空间、`cc-hud.local` 和旧 v1–v6 协议，避免升级破坏配网和控制功能。
- 默认面向 Codex，保留通用推送和旧客户端兼容代码。未知指标必须显示未知，不能沿用 Claude 额度或虚构费用。
- 图标通过代码生成；屏幕中文必须纳入字体子集。更新资源时同时保留生成脚本。

## 验证

```bash
cd host
.venv/bin/python -m pytest -q
cd ../firmware
pio test -e native
pio run -e esp32s3_nano
```

- BLE 调试前检查现有进程，只停止明确属于本项目的连接进程。
- 固件构建、OTA 传输确认、重启后版本回读、真实功能验收分别记录。
- 安装 Codex hooks 时备份并保留所有第三方 hook；不修改信任记录或绕过信任机制。
- 所有说明、注释和新增用户文案使用简体中文，标识符与官方术语保留原文。
