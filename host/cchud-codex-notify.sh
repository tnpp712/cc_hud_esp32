#!/usr/bin/env bash
# cchud-codex-notify.sh — Codex CLI → CC-HUD adapter.
#
# Codex's only zero-config hook is the `notify` program in config.toml, which
# fires once when a turn COMPLETES (no turn-start event). So the only thing we
# can reliably signal is "Codex just finished a turn". We map that to the
# device's one-shot green "done-pulse" (state 0x04) — a brief, glanceable
# "Codex is done, your move" blink that shows regardless of what else is on
# the ring. (A plain "idle" push would be invisible — idle is the default.)
#
# Codex invokes:  cchud-codex-notify.sh '<event-json>'   (JSON as $1)

set -u

JSON="${1:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PY="$HERE/.venv/bin/python"
PUSH="$HERE/push_state.py"
ADDR="${CCHUD_ADDR:-03265204-7126-B6FE-6F17-6E9621CEA97F}"
TIMEOUT="${CCHUD_STATE_TIMEOUT:-6}"
BLE_LOCK="/tmp/cchud/ble.lock"
mkdir -p /tmp/cchud 2>/dev/null || true

[ -x "$PY" ] && [ -f "$PUSH" ] || exit 0

# Only react to turn completion (the JSON type, when present).
TYPE=""
if command -v jq >/dev/null 2>&1 && [ -n "$JSON" ]; then
    TYPE="$(printf '%s' "$JSON" | jq -r '.type // empty' 2>/dev/null)"
fi
case "$TYPE" in
    ""|*turn-complete*|*turn_complete*|*agent-turn*) : ;;   # fire the pulse
    *) exit 0 ;;                                            # other events: ignore
esac

# Fire the green done-pulse. Background + shared BLE lock so we don't collide
# with the Claude quota/state pushers (macOS allows one connection at a time).
(
    got=0
    for _ in $(seq 1 30); do
        if mkdir "$BLE_LOCK" 2>/dev/null; then got=1; break; fi
        sleep 0.1
    done
    [ "$got" = 1 ] || exit 0
    trap 'rmdir "$BLE_LOCK" 2>/dev/null' EXIT INT TERM
    "$PY" "$PUSH" --address "$ADDR" --state done --timeout "$TIMEOUT" \
        </dev/null >>"/tmp/cchud-codex.log" 2>&1
) &
disown 2>/dev/null || true
exit 0
