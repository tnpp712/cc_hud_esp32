#!/usr/bin/env bash
# cchud-quota-push.sh — 从 Claude Code 的 statusline JSON 取「官方用量百分比」
# 和会话开销,交给 cchud-update.sh 做限频、后台、串行化的 BLE 推送。
#
# 由 statusline 包装脚本(cchud-statusline.sh)调用,Claude 的 statusline
# JSON 作为 $1 传入。
#
# 数据来源(全部来自同一份 statusline JSON,与 Claude 桌面 App 的 Usage
# 面板同源,因此 HUD 上的百分比与面板一致):
#   .rate_limits.five_hour.used_percentage   → 5h 已用 %(面板 Current session)
#   .rate_limits.seven_day.used_percentage   → 7d 已用 %(面板 Weekly All models)
#   .rate_limits.*.resets_at                 → 重置时间(epoch 秒)→ 倒计时
#   .context_window.used_percentage          → 上下文窗口已用 %
#   .cost.*                                  → 会话花费 / 时长 / 增删行
#
# 额度条按「百分比」表达:used = 官方 %,limit 固定 100。无需 ccusage,无需
# 估算上限,与官方面板严格一致。BLE 推送的限频由 cchud-update.sh(默认 30s)
# 负责;多个并发会话推的是账号级同一份数字,谁先推都一样。
#
# 环境变量:
#   CCHUD_ADDR    BLE 地址/UUID(默认已写死本机设备)
#   CCHUD_TITLE   HUD 顶部标题(默认沿用 cchud-update.sh 的 "CC HUD")

set -u
HOST="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JSON="${1:-}"

export CCHUD_ADDR="${CCHUD_ADDR:-EC875770-C5EA-539F-2A65-EAF348861E29}"

command -v jq >/dev/null 2>&1 || exit 0
[ -n "$JSON" ] || exit 0

NOW="$(date +%s)"

# 一次 jq 把所有字段抽出来(缺字段一律给 0,兼容老版本 Claude Code)。
read -r used_5h reset5_at used_7d reset7_at ctx_pct cost_usd dur_ms added removed <<EOF
$(printf '%s' "$JSON" | jq -r '
  [ (.rate_limits.five_hour.used_percentage // 0)
  , (.rate_limits.five_hour.resets_at       // 0)
  , (.rate_limits.seven_day.used_percentage // 0)
  , (.rate_limits.seven_day.resets_at       // 0)
  , (.context_window.used_percentage        // 0)
  , (.cost.total_cost_usd                   // 0)
  , (.cost.total_duration_ms                // 0)
  , (.cost.total_lines_added                // 0)
  , (.cost.total_lines_removed              // 0)
  ] | @tsv' 2>/dev/null)
EOF

# 整数化关键字段(jq 的 // 0 已兜底,这里再防脏值)。
abs5="${reset5_at%%.*}"; abs7="${reset7_at%%.*}"
for v in used_5h used_7d ctx_pct added removed abs5 abs7; do
    eval "x=\${$v}"; case "$x" in ''|*[!0-9]*) eval "$v=0";; esac
done
dur_s=$(( ${dur_ms%%.*} / 1000 )); case "$dur_s" in ''|*[!0-9]*) dur_s=0;; esac

# ── 跨会话取最大值合并 ───────────────────────────────────────────────────────
# rate_limits 是账号级数字,但每个会话各自缓存、空闲会话会陈旧。同一窗口内
# 用量单调递增,所以「所有会话近期上报的最大值」= 最新真值。用 resets_at 区分
# 窗口:incoming 窗口更新(resets_at 更大)→ 直接替换(处理重置归零);同窗口
# → 取较大;incoming 是旧窗口 → 忽略。共享状态加 mkdir 锁防并发读改写竞争。
SDIR="/tmp/cchud"; mkdir -p "$SDIR" 2>/dev/null || true
QS="$SDIR/quota.state"; QLOCK="$SDIR/quota-state.lock"
got=0; for _ in $(seq 1 30); do mkdir "$QLOCK" 2>/dev/null && { got=1; break; }; sleep 0.05; done
if [ "$got" = 1 ]; then
    s5p=0; s5r=0; s7p=0; s7r=0
    [ -f "$QS" ] && read -r s5p s5r s7p s7r < "$QS" 2>/dev/null
    for v in s5p s5r s7p s7r; do eval "x=\${$v}"; case "$x" in ''|*[!0-9]*) eval "$v=0";; esac; done
    if   [ "$abs5" -gt "$s5r" ]; then s5p="$used_5h"; s5r="$abs5"
    elif [ "$abs5" -eq "$s5r" ] && [ "$used_5h" -gt "$s5p" ]; then s5p="$used_5h"; fi
    if   [ "$abs7" -gt "$s7r" ]; then s7p="$used_7d"; s7r="$abs7"
    elif [ "$abs7" -eq "$s7r" ] && [ "$used_7d" -gt "$s7p" ]; then s7p="$used_7d"; fi
    printf '%s %s %s %s\n' "$s5p" "$s5r" "$s7p" "$s7r" > "$QS" 2>/dev/null
    rmdir "$QLOCK" 2>/dev/null
    used_5h="$s5p"; abs5="$s5r"; used_7d="$s7p"; abs7="$s7r"
fi

# 倒计时换算(用合并后的绝对 resets_at)。
reset_5h=0; [ "$abs5" -gt "$NOW" ] 2>/dev/null && reset_5h=$(( abs5 - NOW ))
reset_7d=0; [ "$abs7" -gt "$NOW" ] 2>/dev/null && reset_7d=$(( abs7 - NOW ))

export CCHUD_MODE="sub"
export CCHUD_CTX_PCT="$ctx_pct"
export CCHUD_COST_USD="$cost_usd"
export CCHUD_DURATION_S="$dur_s"
export CCHUD_LINES_ADDED="$added"
export CCHUD_LINES_REMOVED="$removed"

# 额度条用百分比:used = 官方 %,limit = 100。
exec "$HOST/cchud-update.sh" "$used_5h" 100 "$used_7d" 100 "$reset_5h" "$reset_7d"
