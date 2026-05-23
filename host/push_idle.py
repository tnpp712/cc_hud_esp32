#!/usr/bin/env python3
"""
push_idle.py — push wall-clock time + a status string to a CC-HUD device
via the v4 BLE payload.

Layout of the 8 + status_len byte payload (little-endian):

    offset 0:  u8  msg_type = 0x04
    offset 1:  u32 unix_ts (UTC seconds since epoch)
    offset 5:  i16 utc_offset_min (local timezone offset in minutes)
    offset 7:  u8  status_len
    offset 8+: status_len bytes ASCII free-form status (weather, etc.)

The firmware keeps its own millis() running and uses unix_ts + (now -
capture_time) so the clock is live between pushes; you can push every
5–10 minutes and the displayed time stays accurate.

CLI:
    push_idle.py --address ADDR [--status "晴 25°C"] [--verbose]
                 [--timeout SECS]

Status is optional. If omitted, the firmware will display only the
clock + date.
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import sys
import time

from bleak import BleakClient

QUOTA_CHAR = "12345678-aaaa-bbbb-cccc-1234567890a1"
MSG_TYPE_V4 = 0x04
MAX_STATUS_LEN = 32


def _log(msg: str, verbose: bool) -> None:
    if verbose:
        print(msg, file=sys.stderr)


def _pack_v4(unix_ts: int, utc_offset_min: int, status: str) -> bytes:
    status_b = status.encode("ascii", errors="replace")[:MAX_STATUS_LEN]
    return struct.pack("<BIhB", MSG_TYPE_V4, unix_ts, utc_offset_min, len(status_b)) + status_b


async def run(address: str, status: str, timeout: float, verbose: bool) -> int:
    # UTC Unix timestamp + local timezone offset in minutes (e.g. +480 for UTC+8).
    unix_ts = int(time.time())
    # time.timezone is seconds WEST of UTC (sign-inverted vs typical "+offset").
    # During DST, time.altzone applies. Pick whichever is currently active.
    is_dst = time.localtime().tm_isdst > 0
    offset_seconds = -(time.altzone if is_dst else time.timezone)
    utc_offset_min = offset_seconds // 60

    payload = _pack_v4(unix_ts, utc_offset_min, status)
    _log(
        f"v4 payload {len(payload)} bytes (unix={unix_ts}, tz={utc_offset_min}min, "
        f"status={status!r})",
        verbose,
    )

    try:
        async with BleakClient(address, timeout=timeout) as client:
            _log("connected, writing…", verbose)
            await client.write_gatt_char(QUOTA_CHAR, payload, response=True)
            _log("done", verbose)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Push wall-clock time + status string to a CC-HUD device.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--address", required=True,
                   help="BLE address / macOS peripheral UUID of the device")
    p.add_argument("--status", default="",
                   help="Free-form ASCII status string, ≤32 chars "
                        "(weather, mood, pomodoro, anything)")
    p.add_argument("--timeout", type=float, default=10.0)
    p.add_argument("--verbose", action="store_true", default=False)
    return p


def main() -> None:
    args = build_parser().parse_args()
    try:
        sys.exit(asyncio.run(run(
            address=args.address,
            status=args.status,
            timeout=args.timeout,
            verbose=args.verbose,
        )))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
