#!/usr/bin/env python3
"""
push_quota.py — Write Claude Code quota numbers to a CC-HUD BLE device.

This is transport-only. Callers are responsible for determining the current
quota numbers and passing them in as arguments.

Three modes:
  * push by scan        : `--5h-used N --5h-limit M --7d-used N --7d-limit M`
  * push by MAC/UUID    : add `--address AA:BB:CC:DD:EE:FF` (skips scan, faster)
  * discover only       : `--discover` — scan and list all CC-HUD devices, no write

On macOS, `--address` is the CoreBluetooth peripheral UUID (e.g.
`6C3E5D24-...`), not a true MAC, because Apple hides MAC addresses from apps.
Use `--discover` once to find it, then save it in your wrapper script.
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
STATE_CHAR_UUID = "12345678-aaaa-bbbb-cccc-1234567890a2"
DEFAULT_DEVICE_NAME = "CC-HUD"

# ── helpers ────────────────────────────────────────────────────────────────────


def _log(msg: str, verbose: bool) -> None:
    if verbose:
        print(msg, file=sys.stderr)


def _u16(value: int, name: str) -> int:
    if not 0 <= value <= 65535:
        raise argparse.ArgumentTypeError(
            f"{name} must be in [0, 65535], got {value}"
        )
    return value


def make_u16_type(name: str):
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


def _pack_payload(h5_used: int, h5_limit: int,
                  d7_used: int, d7_limit: int) -> bytes:
    """Pack 9-byte BLE quota payload (LE u8 + 4× u16)."""
    return struct.pack("<BHHHH", 0x01, h5_used, h5_limit, d7_used, d7_limit)


# ── discover ───────────────────────────────────────────────────────────────────


async def discover(device_name: str, timeout: float, verbose: bool) -> int:
    """
    Scan for *timeout* seconds and print every advertisement whose name
    matches *device_name*. Returns 0 if at least one found, 1 otherwise.
    """
    _log(f"scanning {timeout}s for {device_name!r}…", verbose)
    found: list[tuple[str, str, int]] = []  # (address, name, rssi)

    def detection_callback(device: bleak.BLEDevice, adv_data) -> None:
        # Match either the GAP name in the device or in the advertisement.
        adv_name = adv_data.local_name or device.name
        if adv_name == device_name:
            entry = (device.address, adv_name, adv_data.rssi)
            # de-dup by address
            for existing in found:
                if existing[0] == device.address:
                    return
            found.append(entry)
            _log(f"  found {device.address}  rssi={adv_data.rssi}", verbose)

    async with BleakScanner(detection_callback) as scanner:
        await asyncio.sleep(timeout)

    if not found:
        print(
            f"No devices named {device_name!r} discovered within {timeout}s.",
            file=sys.stderr,
        )
        print(
            "Hints: make sure the board is powered, advertising, and your "
            "terminal has macOS Bluetooth permission.",
            file=sys.stderr,
        )
        return 1

    # Format: tab-separated, easy to grep/parse from a wrapper script.
    print(f"{'ADDRESS':<40}  {'NAME':<16}  RSSI")
    for addr, name, rssi in found:
        print(f"{addr:<40}  {name:<16}  {rssi} dBm")
    return 0


# ── push ───────────────────────────────────────────────────────────────────────


async def push(
    h5_used: int,
    h5_limit: int,
    d7_used: int,
    d7_limit: int,
    address: Optional[str],
    device_name: str,
    timeout: float,
    verbose: bool,
) -> int:
    """
    Write the quota payload. If *address* is given, connect directly (no scan).
    Otherwise scan for a device whose advertised name == *device_name*.

    Returns: 0 success, 1 not found, 2 connect failed, 3 write failed.
    """
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

    target: object  # str | bleak.BLEDevice
    if address:
        # Direct-connect path: bleak accepts the address/UUID string itself.
        _log(f"direct-connect to {address} (no scan)", verbose)
        target = address
    else:
        _log(f"scanning {timeout}s for {device_name!r}…", verbose)
        device = await BleakScanner.find_device_by_name(
            device_name, timeout=timeout
        )
        if device is None:
            print(
                f"ERROR: device {device_name!r} not found within {timeout}s",
                file=sys.stderr,
            )
            return 1
        _log(f"found at {device.address}", verbose)
        target = device

    _log("connecting…", verbose)
    try:
        async with BleakClient(target, timeout=timeout) as client:
            payload = _pack_payload(h5_used, h5_limit, d7_used, d7_limit)
            _log(f"writing {len(payload)} bytes to {QUOTA_CHAR_UUID}", verbose)
            try:
                # response=True is REQUIRED on macOS + NimBLE: with response=False
                # bleak's CoreBluetooth backend silently drops the packet and the
                # ESP32-S3's onWrite callback never fires. Verified empirically
                # during bring-up. The 9-byte payload + ACK round-trip is still
                # <30ms over BLE so there's no real cost.
                await client.write_gatt_char(
                    QUOTA_CHAR_UUID, payload, response=True
                )
            except Exception as exc:
                print(f"ERROR: write failed: {exc}", file=sys.stderr)
                return 3
            _log("disconnecting", verbose)
    except Exception as exc:
        print(f"ERROR: connect failed: {exc}", file=sys.stderr)
        return 2

    _log("done", verbose)
    return 0


# ── CLI ────────────────────────────────────────────────────────────────────────


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Push quota numbers to a CC-HUD BLE device, or discover "
                    "devices on the network.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument(
        "--discover",
        action="store_true",
        default=False,
        help="Scan and print all CC-HUD devices, then exit. "
             "Use this once to find the --address value for your device.",
    )
    p.add_argument(
        "--address",
        dest="address",
        default=None,
        metavar="ADDR",
        help="BLE address (Linux/Win MAC, macOS peripheral UUID). "
             "When given, skips scanning — much faster reconnect.",
    )
    p.add_argument(
        "--device-name",
        dest="device_name",
        default=DEFAULT_DEVICE_NAME,
        help="BLE advertisement name to look for (used by --discover and by "
             "the scan fallback when --address is not given).",
    )
    p.add_argument(
        "--timeout",
        dest="timeout",
        type=float,
        default=10,
        metavar="SECS",
        help="Scan/connect timeout in seconds",
    )
    p.add_argument(
        "--verbose",
        action="store_true",
        default=False,
        help="Print progress steps to stderr",
    )

    # Quota fields are required only in push mode. We mark them optional here
    # and validate manually in main() so --discover can be called alone.
    p.add_argument("--5h-used",  dest="h5_used",
                   type=make_u16_type("--5h-used"),
                   metavar="N", default=None,
                   help="Tokens/requests used in the rolling 5-hour window")
    p.add_argument("--5h-limit", dest="h5_limit",
                   type=make_u16_type("--5h-limit"),
                   metavar="M", default=None,
                   help="Token/request limit for the rolling 5-hour window")
    p.add_argument("--7d-used",  dest="d7_used",
                   type=make_u16_type("--7d-used"),
                   metavar="N", default=None,
                   help="Tokens/requests used in the rolling 7-day window")
    p.add_argument("--7d-limit", dest="d7_limit",
                   type=make_u16_type("--7d-limit"),
                   metavar="M", default=None,
                   help="Token/request limit for the rolling 7-day window")
    return p


async def amain() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.discover:
        return await discover(args.device_name, args.timeout, args.verbose)

    # Push mode: require all four quota fields.
    missing = [n for n, v in [
        ("--5h-used",  args.h5_used),
        ("--5h-limit", args.h5_limit),
        ("--7d-used",  args.d7_used),
        ("--7d-limit", args.d7_limit),
    ] if v is None]
    if missing:
        print(
            "ERROR: push mode requires all four quota arguments. Missing: "
            + ", ".join(missing),
            file=sys.stderr,
        )
        return 2

    return await push(
        h5_used=args.h5_used,
        h5_limit=args.h5_limit,
        d7_used=args.d7_used,
        d7_limit=args.d7_limit,
        address=args.address,
        device_name=args.device_name,
        timeout=args.timeout,
        verbose=args.verbose,
    )


def main() -> None:
    try:
        sys.exit(asyncio.run(amain()))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
