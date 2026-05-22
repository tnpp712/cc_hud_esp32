#!/usr/bin/env python3
"""
push_quota.py — Write Claude Code quota numbers to a CC-HUD BLE device.

This is transport-only. Callers are responsible for determining the current
quota numbers and passing them in as arguments.
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import sys
from typing import Optional

import bleak
from bleak import BleakClient, BleakScanner

# ── BLE constants ──────────────────────────────────────────────────────────────
SERVICE_UUID = "12345678-aaaa-bbbb-cccc-1234567890ab"
QUOTA_CHAR_UUID = "12345678-aaaa-bbbb-cccc-1234567890a1"

# ── helpers ────────────────────────────────────────────────────────────────────


def _log(msg: str, verbose: bool) -> None:
    """Write a status line to stderr (only when verbose is True)."""
    if verbose:
        print(msg, file=sys.stderr)


def _u16(value: int, name: str) -> int:
    """Validate that *value* fits in a uint16 range [0, 65535]."""
    if not 0 <= value <= 65535:
        raise argparse.ArgumentTypeError(
            f"{name} must be in [0, 65535], got {value}"
        )
    return value


def make_u16_type(name: str):
    """Return an argparse *type* callable that validates a uint16."""
    def _parse(raw: str) -> int:
        try:
            v = int(raw)
        except ValueError:
            raise argparse.ArgumentTypeError(
                f"{name} must be an integer, got {raw!r}"
            )
        return _u16(v, name)
    _parse.__name__ = f"u16:{name}"
    return _parse


# ── async core ─────────────────────────────────────────────────────────────────


async def run(
    h5_used: int,
    h5_limit: int,
    d7_used: int,
    d7_limit: int,
    device_name: str,
    timeout: float,
    verbose: bool,
) -> int:
    """
    Scan for *device_name*, connect, write the quota payload, disconnect.

    Returns an exit code: 0 success, 1 not found, 2 connect failed, 3 write failed.
    """
    # ── validation warnings ──────────────────────────────────────────────────
    if h5_used > h5_limit:
        print(
            f"WARNING: 5h used ({h5_used}) exceeds limit ({h5_limit})",
            file=sys.stderr,
        )
    if d7_used > d7_limit:
        print(
            f"WARNING: 7d used ({d7_used}) exceeds limit ({d7_limit})",
            file=sys.stderr,
        )

    # ── scan ─────────────────────────────────────────────────────────────────
    _log(f"scanning for {device_name!r} (timeout {timeout}s)…", verbose)
    device: Optional[bleak.BLEDevice] = await BleakScanner.find_device_by_name(
        device_name, timeout=timeout
    )
    if device is None:
        print(
            f"ERROR: device {device_name!r} not found within {timeout}s",
            file=sys.stderr,
        )
        return 1

    _log(f"found at {device.address}", verbose)

    # ── connect ───────────────────────────────────────────────────────────────
    _log("connecting…", verbose)
    try:
        async with BleakClient(device) as client:
            # ── write ─────────────────────────────────────────────────────────
            payload: bytes = struct.pack(
                "<BHHHH", 0x01, h5_used, h5_limit, d7_used, d7_limit
            )
            _log(f"writing {len(payload)} bytes to {QUOTA_CHAR_UUID}", verbose)
            try:
                await client.write_gatt_char(
                    QUOTA_CHAR_UUID, payload, response=False
                )
            except Exception as exc:
                print(f"ERROR: write failed: {exc}", file=sys.stderr)
                return 3

            _log("disconnecting", verbose)
        # BleakClient context manager disconnects on exit
    except Exception as exc:
        print(f"ERROR: connect failed: {exc}", file=sys.stderr)
        return 2

    _log("done", verbose)
    return 0


# ── CLI ────────────────────────────────────────────────────────────────────────


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Push quota numbers to a CC-HUD BLE device.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument(
        "--5h-used",
        dest="h5_used",
        type=make_u16_type("--5h-used"),
        required=True,
        metavar="N",
        help="Tokens/requests used in the rolling 5-hour window [0-65535]",
    )
    p.add_argument(
        "--5h-limit",
        dest="h5_limit",
        type=make_u16_type("--5h-limit"),
        required=True,
        metavar="M",
        help="Token/request limit for the rolling 5-hour window [0-65535]",
    )
    p.add_argument(
        "--7d-used",
        dest="d7_used",
        type=make_u16_type("--7d-used"),
        required=True,
        metavar="N",
        help="Tokens/requests used in the rolling 7-day window [0-65535]",
    )
    p.add_argument(
        "--7d-limit",
        dest="d7_limit",
        type=make_u16_type("--7d-limit"),
        required=True,
        metavar="M",
        help="Token/request limit for the rolling 7-day window [0-65535]",
    )
    p.add_argument(
        "--device-name",
        dest="device_name",
        default="CC-HUD",
        help="BLE advertisement name of the target device",
    )
    p.add_argument(
        "--timeout",
        dest="timeout",
        type=float,
        default=10,
        metavar="SECS",
        help="Scan timeout in seconds",
    )
    p.add_argument(
        "--verbose",
        action="store_true",
        default=False,
        help="Print progress steps to stderr",
    )
    return p


async def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    code = await run(
        h5_used=args.h5_used,
        h5_limit=args.h5_limit,
        d7_used=args.d7_used,
        d7_limit=args.d7_limit,
        device_name=args.device_name,
        timeout=args.timeout,
        verbose=args.verbose,
    )
    sys.exit(code)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(130)
