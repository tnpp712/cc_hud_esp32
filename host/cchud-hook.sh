#!/usr/bin/env bash
# cchud-hook.sh — Claude Code app-state hook with multi-session aggregation.
#
# Several Claude Code sessions can run at once. Each session's hooks fire
# independently, but the CC-HUD screen can only show one state. Naively
# pushing each session's state would make them clobber each other (session
# A is running a tool while session B finishes and pushes idle → screen
# wrongly goes green). So instead of pushing directly:
#
#   1. Each hook writes ITS OWN session's state to a per-session file
#      keyed by the session_id from the hook's stdin JSON.
#   2. Whichever invocation wins a single global BLE lock AGGREGATES all
#      live sessions (waiting > tool > thinking > idle) and pushes the
#      combined state once. The lock also serialises against the quota
#      pusher (cchud-update.sh) so the two never hit CoreBluetooth at the
#      same time (macOS allows only one connection per peripheral).
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
#   CCHUD_STATE_LOG        log file (default /tmp/cchud-state.log)
#   CCHUD_STATE_TIMEOUT    BLE connect timeout seconds (default 6)
#   CCHUD_SESS_TTL         seconds a session file stays "live" (default 300)

set -u

STATE="${1:-idle}"

# Read stdin once: we need session_id for every event, and tool_name when
# state=tool. Hooks always return in well under 10 ms, so this is cheap.
STDIN="$(cat 2>/dev/null || true)"
SID=""
DETAIL=""
if command -v jq >/dev/null 2>&1 && [ -n "$STDIN" ]; then
    SID="$(printf '%s' "$STDIN" | jq -r '.session_id // empty' 2>/dev/null)"
    if [ "$STATE" = "tool" ]; then
        DETAIL="$(printf '%s' "$STDIN" | jq -r '.tool_name // empty' 2>/dev/null)"
    fi
    # Notification fires for TWO very different things:
    #   1. a real permission / approval request  → genuinely "needs you" (red)
    #   2. a benign "Claude is waiting for your input" at end-of-turn / in
    #      auto mode → NOT urgent
    # Mapping both to "waiting" makes the light cry wolf (red when you're
    # not actually needed). Inspect .message and only keep "waiting" for a
    # genuine permission request; otherwise downgrade to idle.
    if [ "$STATE" = "waiting" ]; then
        MSG="$(printf '%s' "$STDIN" | jq -r '.message // empty' 2>/dev/null)"
        case "$MSG" in
            *permission*|*Permission*|*approve*|*Approve*|*"needs your"*|*confirm*|*Confirm*) : ;;
            *) STATE="idle" ;;
        esac
    fi
fi
[ -n "$SID" ] || SID="default"

LOG="${CCHUD_STATE_LOG:-/tmp/cchud-state.log}"
TIMEOUT="${CCHUD_STATE_TIMEOUT:-6}"
SESS_TTL="${CCHUD_SESS_TTL:-300}"

BASE="/tmp/cchud"
SESS_DIR="$BASE/sessions"
BLE_LOCK="$BASE/ble.lock"
LAST_PUSH="$BASE/last-pushed.state"
mkdir -p "$SESS_DIR" 2>/dev/null || true

# Record THIS session's latest state (atomic-ish: small write). mtime is
# the liveness signal used by the aggregator's TTL sweep.
printf '%s\t%s' "$STATE" "$DETAIL" > "$SESS_DIR/$SID.state"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$PROJECT_ROOT/host/.venv/bin/python"
PUSH="$PROJECT_ROOT/host/push_state.py"
ADDR="${CCHUD_ADDR:-03265204-7126-B6FE-6F17-6E9621CEA97F}"

if [ ! -x "$PY" ] || [ ! -f "$PUSH" ]; then
    echo "$(date +%H:%M:%S) cchud-hook: missing $PY or $PUSH" >>"$LOG"
    exit 3
fi

# Aggregate all live sessions into one (state, detail). Expired session
# files (no update within SESS_TTL) are swept — a session that goes quiet
# is treated as gone, which is correct: a busy session keeps refreshing
# its file's mtime, an idle/dead one does not.
aggregate() {
    local now best_p=-1 best_s="idle" best_d="" f mt age st det p
    local total=0 busy=0
    now="$(date +%s)"
    for f in "$SESS_DIR"/*.state; do
        [ -e "$f" ] || continue
        mt="$(stat -f %m "$f" 2>/dev/null || echo 0)"
        age=$(( now - mt ))
        if [ "$age" -gt "$SESS_TTL" ]; then rm -f "$f"; continue; fi
        IFS=$'\t' read -r st det < "$f"
        total=$(( total + 1 ))
        case "$st" in
            waiting) p=3; busy=$(( busy + 1 )) ;;
            tool)    p=2; busy=$(( busy + 1 )) ;;
            thinking) p=1; busy=$(( busy + 1 )) ;;
            *)       p=0 ;;
        esac
        if [ "$p" -gt "$best_p" ]; then best_p="$p"; best_s="$st"; best_d="$det"; fi
    done
    # Emit fields separated by US (0x1f, a NON-whitespace control char) so a
    # downstream `read` preserves EMPTY fields. Tab is whitespace, and `read`
    # with a whitespace IFS collapses consecutive separators — which dropped
    # the empty `detail` field for idle/thinking states and shifted
    # total/busy into it (the "detail=1" bug).
    printf '%s\x1f%s\x1f%s\x1f%s' "$best_s" "$best_d" "$total" "$busy"
}

# Fire the BLE push in the background so the hook returns immediately.
# Only the lock holder pushes; others rely on the holder re-aggregating
# (the push-until-stable loop) to pick up their just-written state.
(
    if ! mkdir "$BLE_LOCK" 2>/dev/null; then
        exit 0   # someone is pushing; their re-aggregation will see our file
    fi
    trap 'rmdir "$BLE_LOCK" 2>/dev/null' EXIT INT TERM

    # Push until the aggregate stops changing (catches states that landed
    # while we were mid-push), capped at 3 rounds so we never spin.
    for _ in 1 2 3; do
        agg="$(aggregate)"
        IFS=$'\x1f' read -r a_state a_detail a_total a_busy <<< "$agg"
        key="$a_state:$a_detail:$a_total:$a_busy"
        prev="$(cat "$LAST_PUSH" 2>/dev/null || echo "")"
        [ "$key" = "$prev" ] && break

        "$PY" "$PUSH" \
            --address "$ADDR" \
            --state "$a_state" \
            --detail "$a_detail" \
            --total-sessions "${a_total:-0}" \
            --busy-sessions "${a_busy:-0}" \
            --timeout "$TIMEOUT" \
            </dev/null >>"$LOG" 2>&1
        printf '%s' "$key" > "$LAST_PUSH"
        sleep 0.3   # let any concurrent session writes land, then re-check
    done
) &
disown 2>/dev/null || true

exit 0
