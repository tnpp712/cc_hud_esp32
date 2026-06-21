#!/usr/bin/env bash
# cchud-codex-dispatch.sh — fan-out for Codex's single `notify` slot.
#
# Codex allows only ONE notify program. The user already had a computer-use
# notify (SkyComputerUseClient turn-ended). This dispatcher preserves that
# AND adds the CC-HUD adapter, so both fire on every Codex turn.
#
# Codex invokes:  cchud-codex-dispatch.sh '<event-json>'   (JSON as $1)

set -u

JSON="${1:-}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 1. Preserve the original computer-use notify (same call Codex used to make).
ORIG="/Users/jin/.codex/computer-use/Codex Computer Use.app/Contents/SharedSupport/SkyComputerUseClient.app/Contents/MacOS/SkyComputerUseClient"
if [ -x "$ORIG" ]; then
    "$ORIG" "turn-ended" "$JSON" >/dev/null 2>&1 &
fi

# 2. CC-HUD status-light adapter.
"$HERE/cchud-codex-notify.sh" "$JSON" >/dev/null 2>&1 &

wait
exit 0
