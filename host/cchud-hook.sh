#!/usr/bin/env bash
# cchud-hook.sh — single Claude Code hook entry. Reads the state from
# the first arg (and tool_name from stdin JSON when state=tool),
# de-duplicates against the previous push, and fires a BLE write in
# the background so the hook returns in <10 ms.
#
# Configure in ~/.claude/settings.json:
#   UserPromptSubmit → cchud-hook.sh thinking
#   PreToolUse       → cchud-hook.sh tool
#   PostToolUse      → cchud-hook.sh thinking
#   Stop             → cchud-hook.sh idle
#   Notification     → cchud-hook.sh waiting
#
# Env overrides:
#   CCHUD_ADDR             BLE address/UUID of the device
#   CCHUD_STATE_LAST       dedup cache file (default /tmp/cchud-state-last)
#   CCHUD_STATE_LOG        log file (default /tmp/cchud-state.log)
#   CCHUD_STATE_TIMEOUT    BLE connect timeout seconds (default 6)

set -u

STATE="${1:-idle}"

DETAIL=""
if [ "$STATE" = "tool" ]; then
    STDIN="$(cat 2>/dev/null || true)"
    if [ -n "$STDIN" ] && command -v jq >/dev/null 2>&1; then
        DETAIL="$(printf '%s' "$STDIN" | jq -r '.tool_name // empty' 2>/dev/null)"
    fi
fi

LAST_FILE="${CCHUD_STATE_LAST:-/tmp/cchud-state-last}"
LOG="${CCHUD_STATE_LOG:-/tmp/cchud-state.log}"
TIMEOUT="${CCHUD_STATE_TIMEOUT:-6}"

# Dedup: identical state+detail → skip (hooks fire dozens of times per
# session; BLE only needs the transitions).
KEY="$STATE:$DETAIL"
if [ -e "$LAST_FILE" ]; then
    PREV="$(cat "$LAST_FILE" 2>/dev/null)"
    if [ "$PREV" = "$KEY" ]; then
        exit 0
    fi
fi

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$PROJECT_ROOT/host/.venv/bin/python"
PUSH="$PROJECT_ROOT/host/push_state.py"
ADDR="${CCHUD_ADDR:-5BCF7865-FCCD-F66E-CBCC-19B5978C6902}"

if [ ! -x "$PY" ] || [ ! -f "$PUSH" ]; then
    echo "$(date +%H:%M:%S) cchud-hook: missing $PY or $PUSH" >>"$LOG"
    exit 3
fi

# Cache the new key BEFORE forking so back-to-back hook calls dedup.
echo "$KEY" > "$LAST_FILE"

# Fire-and-forget background push. setsid + disown so the hook returns
# instantly even if BLE takes a second to connect.
(
    "$PY" "$PUSH" \
        --address "$ADDR" \
        --state "$STATE" \
        --detail "$DETAIL" \
        --timeout "$TIMEOUT" \
        </dev/null >>"$LOG" 2>&1
) &
disown 2>/dev/null || true

exit 0
