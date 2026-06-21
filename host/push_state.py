#!/usr/bin/env python3
"""
push_state.py — push the current Claude Code app-state to a CC-HUD device.

msg_type 0x07 payload:
    offset 0:  u8 msg_type = 0x07
    offset 1:  u8 state      (0=idle, 1=thinking, 2=tool, 3=waiting)
    offset 2:  u8 detail_len (0..15)
    offset 3+: ASCII detail bytes (tool name when state=tool)

CLI:
    push_state.py --address ADDR --state thinking
    push_state.py --address ADDR --state tool --detail Bash
    push_state.py --address ADDR --state idle
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import sys

from bleak import BleakClient

QUOTA_CHAR = "12345678-aaaa-bbbb-cccc-1234567890a1"
MSG_TYPE_STATE = 0x07
DETAIL_MAX = 15

STATE_MAP = {
    "idle":     0,
    "thinking": 1,
    "tool":     2,
    "waiting":  3,
    "done":     4,   # one-shot → green done-pulse on the ring, then idle
}


def pack_state(state: str, detail: str,
               total_sessions: int = 0, busy_sessions: int = 0) -> bytes:
    s = STATE_MAP.get(state)
    if s is None:
        raise ValueError(f"unknown state {state!r}, expected one of "
                         f"{list(STATE_MAP)}")
    d = detail.encode("ascii", errors="replace")[:DETAIL_MAX]
    base = struct.pack("<BBB", MSG_TYPE_STATE, s, len(d)) + d
    # Optional stage-3 session-count bytes. The firmware reads them only
    # when present, so older firmware ignores them harmlessly.
    base += struct.pack("<BB", min(255, max(0, total_sessions)),
                        min(255, max(0, busy_sessions)))
    return base


async def run(address: str, state: str, detail: str, timeout: float,
              total_sessions: int = 0, busy_sessions: int = 0) -> int:
    payload = pack_state(state, detail, total_sessions, busy_sessions)
    # Retry once on a transient BLE disconnect — CoreBluetooth drops the
    # link intermittently under lock contention with the other pushers.
    last_err = None
    for _ in range(2):
        try:
            async with BleakClient(address, timeout=timeout) as c:
                await c.write_gatt_char(QUOTA_CHAR, payload, response=True)
            return 0
        except Exception as exc:
            last_err = exc
            await asyncio.sleep(0.4)
    print(f"ERROR: {last_err}", file=sys.stderr)
    return 2


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("--address", required=True)
    p.add_argument("--state", required=True, choices=list(STATE_MAP))
    p.add_argument("--detail", default="",
                   help="ASCII tool name when --state=tool (≤15 chars)")
    p.add_argument("--total-sessions", dest="total_sessions",
                   type=int, default=0,
                   help="Live Claude Code session count (stage 3)")
    p.add_argument("--busy-sessions", dest="busy_sessions",
                   type=int, default=0,
                   help="How many sessions are non-idle (stage 3)")
    p.add_argument("--timeout", type=float, default=5.0)
    args = p.parse_args()
    try:
        sys.exit(asyncio.run(run(args.address, args.state,
                                  args.detail, args.timeout,
                                  args.total_sessions, args.busy_sessions)))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
