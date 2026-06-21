#!/usr/bin/env bash
# cchud-update.sh — rate-limited fire-and-forget BLE push to CC-HUD.
#
# Designed to be called from Claude Code statusline.sh (or any other hook
# that fires very frequently). The script:
#   1. Skips immediately if we pushed within $CCHUD_RATE_LIMIT seconds.
#   2. Forks the actual BLE push into the background so the caller returns
#      in <50ms regardless of BLE latency.
#   3. Logs to /tmp/cchud-push.log for later inspection.
#
# Usage (positional, in order):
#   cchud-update.sh <5h_used> <5h_limit> <7d_used> <7d_limit> \
#                   [5h_reset_s] [7d_reset_s]
#
#   In API mode, you typically pass zeros for the 4 quota fields and
#   provide CCHUD_MODE=api with CCHUD_COST_USD and CCHUD_DURATION_S.
#
# Environment overrides:
#   CCHUD_ADDR        BLE address/UUID of the device (required).
#   CCHUD_RATE_LIMIT  minimum seconds between BLE pushes (default 30).
#   CCHUD_LOG         log file path (default /tmp/cchud-push.log).
#   CCHUD_TIMEOUT     bleak scan/connect timeout (default 8 seconds).
#   CCHUD_TITLE       header title text, e.g. "Plan Max (20x)"
#                     (default "CC HUD", max 32 ASCII chars).
#   CCHUD_MODE        "sub" (default) or "api".
#   CCHUD_COST_USD    session cost in USD when CCHUD_MODE=api (default 0).
#   CCHUD_DURATION_S  session duration in seconds when CCHUD_MODE=api (default 0).

set -u

if [ $# -lt 4 ] || [ $# -gt 6 ]; then
    echo "usage: $0 5h_used 5h_limit 7d_used 7d_limit [5h_reset_s] [7d_reset_s]" >&2
    exit 2
fi

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$PROJECT_ROOT/host/.venv/bin/python"
PUSH="$PROJECT_ROOT/host/push_quota.py"

ADDR="${CCHUD_ADDR:-03265204-7126-B6FE-6F17-6E9621CEA97F}"
RATE="${CCHUD_RATE_LIMIT:-30}"
LOG="${CCHUD_LOG:-/tmp/cchud-push.log}"
TIMEOUT="${CCHUD_TIMEOUT:-8}"
TITLE="${CCHUD_TITLE:-CC HUD}"
MODE="${CCHUD_MODE:-sub}"
COST_USD="${CCHUD_COST_USD:-0}"
DURATION_S="${CCHUD_DURATION_S:-0}"
CTX_PCT="${CCHUD_CTX_PCT:-0}"
LINES_ADDED="${CCHUD_LINES_ADDED:-0}"
LINES_REMOVED="${CCHUD_LINES_REMOVED:-0}"

H5_USED="$1"; H5_LIMIT="$2"; D7_USED="$3"; D7_LIMIT="$4"
H5_RESET="${5:-0}"; D7_RESET="${6:-0}"

LAST_FILE="/tmp/cchud-last-push.ts"
NOW="$(date +%s)"

# Rate-limit: skip silently if too recent.
if [ -e "$LAST_FILE" ]; then
    LAST="$(cat "$LAST_FILE" 2>/dev/null || echo 0)"
    if [ "$((NOW - LAST))" -lt "$RATE" ]; then
        exit 0
    fi
fi

if [ ! -x "$PY" ] || [ ! -f "$PUSH" ]; then
    echo "cchud-update: missing $PY or $PUSH; create the venv first" >&2
    exit 3
fi

# Mark timestamp BEFORE forking so concurrent statusline calls don't race.
echo "$NOW" > "$LAST_FILE"

# Shared BLE lock — the SAME lock the app-state hook (cchud-hook.sh) uses.
# macOS CoreBluetooth allows only one connection per peripheral, so the
# quota push and the state push must never run concurrently. mkdir is
# atomic, so it doubles as a cross-script mutex.
BLE_LOCK="/tmp/cchud/ble.lock"
mkdir -p /tmp/cchud 2>/dev/null || true

# Fire and forget. Close stdin, redirect stdout/stderr, & to background.
(
    # Spin-wait briefly for the lock (state pushes are short). If we can't
    # get it in time, drop this push — the next statusline tick retries,
    # and quota data is unchanged between ticks anyway.
    got=0
    for _ in $(seq 1 30); do
        if mkdir "$BLE_LOCK" 2>/dev/null; then got=1; break; fi
        sleep 0.1
    done
    [ "$got" = 1 ] || exit 0
    trap 'rmdir "$BLE_LOCK" 2>/dev/null' EXIT INT TERM

    "$PY" "$PUSH" \
        --address "$ADDR" \
        --5h-used "$H5_USED" --5h-limit "$H5_LIMIT" \
        --7d-used "$D7_USED" --7d-limit "$D7_LIMIT" \
        --5h-reset-in "$H5_RESET" --7d-reset-in "$D7_RESET" \
        --mode "$MODE" \
        --cost-usd "$COST_USD" \
        --duration-s "$DURATION_S" \
        --ctx-pct "$CTX_PCT" \
        --lines-added "$LINES_ADDED" \
        --lines-removed "$LINES_REMOVED" \
        --title "$TITLE" \
        --timeout "$TIMEOUT" \
        --verbose \
        </dev/null >>"$LOG" 2>&1
) &
disown 2>/dev/null || true

exit 0
