#!/usr/bin/env python3
"""
demo_moods.py — cycle through the 5 pet moods by pushing quotas at
different usage percentages, holding each for 8 seconds.

The device shows mood only on the idle screen, so this script also
flips force-idle ON for the duration.

Usage:
    demo_moods.py --address 5BCF7865-...
"""

from __future__ import annotations
import argparse, asyncio, struct, sys, time
from bleak import BleakClient

QUOTA_CHAR = "12345678-aaaa-bbbb-cccc-1234567890a1"

# Each step: label, used_5h, limit_5h, expected_mood
DEMO_STEPS = [
    ("HAPPY        (15%)",  15,  100, "(=^.^=)"),
    ("CALM         (45%)",  45,  100, "(='.'=)"),
    ("TIRED        (70%)",  70,  100, "(=u.u=)"),
    ("STRESSED     (88%)",  88,  100, "(=>.<=)"),
    ("OVERWHELMED  (97%)",  97,  100, "(=T.T=)"),
]

HOLD_SECONDS = 8


def pack_v3(used_5h, limit_5h):
    """Build a v3 quota payload. title='Demo', mode=sub, no resets, no cost."""
    title = b"Demo"
    return struct.pack(
        "<BBHHHHIIIIB",
        0x03,        # msg_type v3
        0,           # mode sub
        used_5h, limit_5h,
        50, 1000,    # 7d 5%
        0, 0,        # reset_in_s 5h/7d
        0, 0,        # cost / dur
        len(title)
    ) + title


def pack_force_idle(enter: bool):
    return bytes([0x05, 0x01 if enter else 0x00])


async def run(address):
    async with BleakClient(address, timeout=15) as c:
        # Force idle so we can see the pet
        await c.write_gatt_char(QUOTA_CHAR, pack_force_idle(True), response=True)
        print("→ idle forced on\n")

        try:
            for label, used, limit, sprite in DEMO_STEPS:
                payload = pack_v3(used, limit)
                await c.write_gatt_char(QUOTA_CHAR, payload, response=True)
                # immediately re-force-idle (quota push reset force flag? no it doesn't)
                print(f"  {label}  →  {sprite}   (holding {HOLD_SECONDS}s)")
                await asyncio.sleep(HOLD_SECONDS)
        finally:
            await c.write_gatt_char(QUOTA_CHAR, pack_force_idle(False), response=True)
            print("\n→ idle forced off")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--address", required=True)
    args = p.parse_args()
    try:
        asyncio.run(run(args.address))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
