#!/usr/bin/env bash
# cchud-idle.sh — rate-limited fire-and-forget BLE idle/time push.
#
# Send the host's wall-clock time + an optional status string to the
# CC-HUD device. Intended to be called alongside the quota wrapper from
# statusline.sh — it shares the same fork-and-forget pattern but uses a
# longer rate limit (default 10 minutes; the firmware extrapolates time
# via millis() between pushes so this is plenty).
#
# Usage:
#   cchud-idle.sh "<status string>"
#
# Environment overrides:
#   CCHUD_ADDR              BLE address/UUID of the device.
#   CCHUD_IDLE_RATE_LIMIT   minimum seconds between idle pushes (default 600).
#   CCHUD_IDLE_LOG          log file path (default /tmp/cchud-idle.log).
#   CCHUD_IDLE_TIMEOUT      bleak scan/connect timeout (default 8 seconds).
#   CCHUD_WEATHER_CITY      if set (e.g. "Beijing"), fetch weather from
#                           wttr.in and use it as the status string,
#                           overriding the positional argument.

set -u

STATUS="${1:-}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$PROJECT_ROOT/host/.venv/bin/python"
PUSH="$PROJECT_ROOT/host/push_idle.py"

ADDR="${CCHUD_ADDR:-03265204-7126-B6FE-6F17-6E9621CEA97F}"
RATE="${CCHUD_IDLE_RATE_LIMIT:-600}"
LOG="${CCHUD_IDLE_LOG:-/tmp/cchud-idle.log}"
TIMEOUT="${CCHUD_IDLE_TIMEOUT:-8}"

LAST_FILE="/tmp/cchud-idle-last-push.ts"
NOW="$(date +%s)"

if [ -e "$LAST_FILE" ]; then
    LAST="$(cat "$LAST_FILE" 2>/dev/null || echo 0)"
    if [ "$((NOW - LAST))" -lt "$RATE" ]; then
        exit 0
    fi
fi

if [ ! -x "$PY" ] || [ ! -f "$PUSH" ]; then
    echo "cchud-idle: missing $PY or $PUSH; create the venv first" >&2
    exit 3
fi

echo "$NOW" > "$LAST_FILE"

# Fire and forget. Background python so the statusline returns instantly.
EXTRA_ARGS=()
if [ -n "${CCHUD_WEATHER_CITY:-}" ]; then
    EXTRA_ARGS+=(--weather-city "$CCHUD_WEATHER_CITY")
fi

"$PY" "$PUSH" \
    --address "$ADDR" \
    --status "$STATUS" \
    "${EXTRA_ARGS[@]}" \
    --timeout "$TIMEOUT" \
    --verbose \
    </dev/null >>"$LOG" 2>&1 &
disown 2>/dev/null || true

exit 0
