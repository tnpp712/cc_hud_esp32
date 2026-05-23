#!/usr/bin/env python3
"""
force_idle.py — toggle the device into / out of the idle (clock) screen
on demand, bypassing the 30-minute auto-idle timer.

Send msg_type 0x05 (1 byte) + 0x01 (enter forced idle) or 0x00 (leave).

Usage:
    force_idle.py --address ADDR --on
    force_idle.py --address ADDR --off

When --on is set, the device shows the idle clock + date + last-pushed
status regardless of recent quota writes. When --off is set, the device
falls back to the normal timing-based behaviour (HUD until 30 min of
no quota writes pass).
"""

from __future__ import annotations

import argparse
import asyncio
import sys

from bleak import BleakClient

QUOTA_CHAR = "12345678-aaaa-bbbb-cccc-1234567890a1"


async def run(address: str, enter: bool, timeout: float) -> int:
    payload = bytes([0x05, 0x01 if enter else 0x00])
    try:
        async with BleakClient(address, timeout=timeout) as c:
            await c.write_gatt_char(QUOTA_CHAR, payload, response=True)
            print(
                "force_idle set to "
                + ("ENTER (always idle screen)" if enter else "LEAVE (normal timing)"),
                file=sys.stderr,
            )
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("--address", required=True)
    grp = p.add_mutually_exclusive_group(required=True)
    grp.add_argument("--on",  dest="enter", action="store_true",
                     help="Force the device into the idle screen now")
    grp.add_argument("--off", dest="enter", action="store_false",
                     help="Release the force-idle latch; back to timing-based behaviour")
    p.add_argument("--timeout", type=float, default=10.0)
    args = p.parse_args()
    try:
        sys.exit(asyncio.run(run(args.address, args.enter, args.timeout)))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
