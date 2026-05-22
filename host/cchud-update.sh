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
# Usage:
#   cchud-update.sh <5h_used> <5h_limit> <7d_used> <7d_limit>
#
# Environment overrides:
#   CCHUD_ADDR        BLE address/UUID of the device.
#                     macOS: peripheral UUID from `push_quota.py --discover`.
#                     Linux/Windows: traditional MAC address.
#   CCHUD_RATE_LIMIT  minimum seconds between BLE pushes (default 30).
#   CCHUD_LOG         path for stdout/stderr of the bleak process
#                     (default /tmp/cchud-push.log).
#   CCHUD_TIMEOUT     bleak scan/connect timeout (default 8 seconds).

set -u

if [ $# -ne 4 ]; then
    echo "usage: $0 5h_used 5h_limit 7d_used 7d_limit" >&2
    echo "       (all four must be non-negative integers <= 65535)" >&2
    exit 2
fi

# Resolve project root from this script's location so `cchud-update.sh` works
# no matter where the caller invokes it from.
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$PROJECT_ROOT/host/.venv/bin/python"
PUSH="$PROJECT_ROOT/host/push_quota.py"

ADDR="${CCHUD_ADDR:-5BCF7865-FCCD-F66E-CBCC-19B5978C6902}"
RATE="${CCHUD_RATE_LIMIT:-30}"
LOG="${CCHUD_LOG:-/tmp/cchud-push.log}"
TIMEOUT="${CCHUD_TIMEOUT:-8}"

LAST_FILE="/tmp/cchud-last-push.ts"
NOW="$(date +%s)"

# Rate-limit: skip silently if too recent.
if [ -e "$LAST_FILE" ]; then
    LAST="$(cat "$LAST_FILE" 2>/dev/null || echo 0)"
    if [ "$((NOW - LAST))" -lt "$RATE" ]; then
        exit 0
    fi
fi

# Sanity: virtualenv + push script must exist.
if [ ! -x "$PY" ] || [ ! -f "$PUSH" ]; then
    echo "cchud-update: missing $PY or $PUSH; run host/setup-venv.sh first" >&2
    exit 3
fi

# Mark this attempt's timestamp BEFORE forking, so concurrent statusline
# invocations don't all race the rate-limit check.
echo "$NOW" > "$LAST_FILE"

# Detach and forget. nohup + disown so we don't wedge on SIGHUP if the
# parent statusline shell goes away.
(
    nohup "$PY" "$PUSH" \
        --address "$ADDR" \
        --5h-used "$1" --5h-limit "$2" \
        --7d-used "$3" --7d-limit "$4" \
        --timeout "$TIMEOUT" \
        >>"$LOG" 2>&1
) &
disown $! 2>/dev/null || true

exit 0
