#!/usr/bin/env bash
# cchud-statusline.sh — Claude Code statusLine 包装器。
#
# 作用:把你原来的 ccstatusline 输出原样渲染出来(终端状态栏不变),同时在
# 后台把这次的 statusline JSON 交给 cchud-quota-push.sh,做限频的 BLE 推送。
#
# 设计原则:绝不阻塞、绝不弄坏状态栏。推送是 fork 到后台的,且吞掉所有
# stdout/stderr;即便 HUD 没连、ccusage 失败,状态栏照常输出。
#
# 在 ~/.claude/settings.json 里把 statusLine.command 指向本脚本即可。

set -u
HOST="/Users/yaoying/github/cc_hud_esp32/host"

# Claude 通过 stdin 把会话 JSON 传进来,只读一次。
INPUT="$(cat)"

# 后台推 HUD(fire-and-forget,与状态栏渲染完全解耦)。
"$HOST/cchud-quota-push.sh" "$INPUT" >/dev/null 2>&1 &
disown 2>/dev/null || true

# 渲染真正的状态栏(沿用原命令)。
printf '%s' "$INPUT" | bunx -y ccstatusline@latest
