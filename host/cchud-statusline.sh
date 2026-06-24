#!/usr/bin/env bash
# cchud-statusline.sh — Claude Code statusLine 包装:渲染状态栏 + 把 JSON 投递给 daemon。
#
# 双职责:(1) 沿用 ccstatusline 渲染终端状态栏;(2) 后台把 statusline JSON
# 透传给 cchud-emit.sh(投递 daemon 采集额度)。绝不阻塞、绝不弄坏状态栏。
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

# Claude 通过 stdin 把会话/额度 JSON 传进来,只读一次。
INPUT="$(cat)"

# 后台投递给 daemon(fire-and-forget,吞掉所有输出)。
printf '%s' "$INPUT" | "$HERE/cchud-emit.sh" claude Status >/dev/null 2>&1 &
disown 2>/dev/null || true

# 渲染真正的状态栏(沿用原命令)。
printf '%s' "$INPUT" | bunx -y ccstatusline@latest
