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
}


def pack_state(state: str, detail: str) -> bytes:
    s = STATE_MAP.get(state)
    if s is None:
        raise ValueError(f"unknown state {state!r}, expected one of "
                         f"{list(STATE_MAP)}")
    d = detail.encode("ascii", errors="replace")[:DETAIL_MAX]
    return struct.pack("<BBB", MSG_TYPE_STATE, s, len(d)) + d


async def run(address: str, state: str, detail: str, timeout: float) -> int:
    payload = pack_state(state, detail)
    try:
        async with BleakClient(address, timeout=timeout) as c:
            await c.write_gatt_char(QUOTA_CHAR, payload, response=True)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


def main() -> None:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("--address", required=True)
    p.add_argument("--state", required=True, choices=list(STATE_MAP))
    p.add_argument("--detail", default="",
                   help="ASCII tool name when --state=tool (≤15 chars)")
    p.add_argument("--timeout", type=float, default=5.0)
    args = p.parse_args()
    try:
        sys.exit(asyncio.run(run(args.address, args.state,
                                  args.detail, args.timeout)))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
