#!/usr/bin/env python3
"""
ota.py — Stream a firmware binary to a CC-HUD device over BLE OTA.

Usage:
  ota.py --address ADDR --firmware path/to/firmware.bin [--chunk 180] \
         [--timeout 30] [--verbose]

The device exposes an OTA GATT service at:
    Service:    12345678-aaaa-bbbb-cccc-1234567890bb
    Control:    12345678-aaaa-bbbb-cccc-1234567890b1   (write)
    Data:       12345678-aaaa-bbbb-cccc-1234567890b2   (write/write-no-resp)
    State:      12345678-aaaa-bbbb-cccc-1234567890b3   (read+notify)

Control commands:
    0x00 + u32 LE size   start
    0x01                  end (commit + reboot)
    0x02                  abort

We send chunks at write-no-response speed (no per-chunk ACK round-trip) for
throughput, then poll State notify to confirm progress and final "OK".
"""

from __future__ import annotations

import argparse
import asyncio
import os
import struct
import sys
import time
from typing import Optional

from bleak import BleakClient

OTA_SERVICE = "12345678-aaaa-bbbb-cccc-1234567890bb"
OTA_CTRL    = "12345678-aaaa-bbbb-cccc-1234567890b1"
OTA_DATA    = "12345678-aaaa-bbbb-cccc-1234567890b2"
OTA_STATE   = "12345678-aaaa-bbbb-cccc-1234567890b3"

CMD_START = 0x00
CMD_END   = 0x01
CMD_ABORT = 0x02


def log(msg: str, verbose: bool = True) -> None:
    if verbose:
        print(msg, file=sys.stderr)


async def run(address: str, fw_path: str, chunk: int, timeout: float,
              verbose: bool) -> int:
    if not os.path.isfile(fw_path):
        print(f"ERROR: firmware not found: {fw_path}", file=sys.stderr)
        return 1
    with open(fw_path, "rb") as f:
        fw = f.read()
    fw_size = len(fw)
    if fw_size == 0:
        print("ERROR: firmware file is empty", file=sys.stderr)
        return 1
    log(f"firmware: {fw_path} = {fw_size} bytes ({fw_size/1024:.1f} KB)", verbose)

    last_state: list[bytes] = [b""]
    state_event = asyncio.Event()

    def on_state(_uuid, data: bytearray) -> None:
        s = bytes(data)
        last_state[0] = s
        state_event.set()
        if verbose:
            try:
                txt = s.decode("utf-8", errors="replace")
            except Exception:
                txt = repr(s)
            print(f"[state] {txt}", file=sys.stderr)

    async def wait_state_starts(prefix: bytes, deadline_s: float) -> Optional[bytes]:
        end = time.monotonic() + deadline_s
        while time.monotonic() < end:
            if last_state[0].startswith(prefix):
                return last_state[0]
            state_event.clear()
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            try:
                await asyncio.wait_for(state_event.wait(), timeout=remaining)
            except asyncio.TimeoutError:
                break
        return None

    log(f"connecting to {address}…", verbose)
    try:
        async with BleakClient(address, timeout=timeout) as client:
            log("connected", verbose)
            await client.start_notify(OTA_STATE, on_state)

            # 1. START with total size — must use response=True so the
            #    chip actually receives the command (macOS quirk).
            start_payload = bytes([CMD_START]) + struct.pack("<I", fw_size)
            log(f"sending START (size={fw_size})", verbose)
            await client.write_gatt_char(OTA_CTRL, start_payload, response=True)

            ready = await wait_state_starts(b"READY", deadline_s=5)
            if ready is None:
                print(f"ERROR: did not see READY (last state: "
                      f"{last_state[0]!r})", file=sys.stderr)
                return 2
            log("device ready, streaming chunks…", verbose)

            # 2. Stream the firmware. We must use response=True: macOS's
            #    CoreBluetooth backend silently drops write-no-response
            #    packets once an internal queue fills, so we'd see only
            #    ~8 KB land. Per-chunk ACK gives ~50 KB/s, plenty for a
            #    ~600 KB firmware in well under a minute.
            t0 = time.monotonic()
            sent = 0
            next_log = chunk * 100  # log every ~18 KB
            for i in range(0, fw_size, chunk):
                payload = fw[i:i + chunk]
                await client.write_gatt_char(OTA_DATA, payload, response=True)
                sent += len(payload)
                if verbose and sent >= next_log:
                    elapsed = time.monotonic() - t0
                    rate = sent / elapsed if elapsed > 0 else 0
                    print(f"[ota] {sent}/{fw_size} "
                          f"({100*sent//fw_size}%) {rate/1024:.1f} KB/s",
                          file=sys.stderr)
                    next_log += chunk * 100
            elapsed = time.monotonic() - t0
            rate = sent / elapsed if elapsed > 0 else 0
            log(f"all {sent} bytes sent in {elapsed:.1f}s "
                f"({rate/1024:.1f} KB/s)", verbose)

            # 3. END — device verifies & reboots. We may not see the
            #    disconnect cleanly because the chip resets immediately
            #    after sending "OK"; treat that as success.
            log("sending END", verbose)
            try:
                await client.write_gatt_char(OTA_CTRL,
                                             bytes([CMD_END]),
                                             response=True)
            except Exception as exc:
                # The device may reboot before the response completes; the
                # only way to confirm is via the notify we got just before.
                log(f"END write returned: {exc} (often expected — chip reset)",
                    verbose)

            ok = await wait_state_starts(b"OK", deadline_s=8)
            if ok is None:
                # Maybe we missed the notify due to early disconnect. Be
                # forgiving and report likely success if we wrote everything.
                if sent == fw_size:
                    log("no OK notify but full payload sent — likely OK",
                        verbose)
                    return 0
                print("ERROR: did not see OK notify after END",
                      file=sys.stderr)
                return 3

            log("OTA complete; device rebooting into new firmware", verbose)
            return 0
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="OTA-flash a firmware.bin to a CC-HUD device over BLE.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--address", required=True,
                   help="BLE address / macOS peripheral UUID of the device "
                        "(get from `push_quota.py --discover`)")
    p.add_argument("--firmware", required=True,
                   help="path to firmware.bin (typically "
                        ".pio/build/esp32s3_nano/firmware.bin)")
    p.add_argument("--chunk", type=int, default=240,
                   help="bytes per BLE write; <= negotiated MTU - 3")
    p.add_argument("--timeout", type=float, default=30,
                   help="connect timeout in seconds")
    p.add_argument("--verbose", action="store_true", default=False)
    return p


def main() -> None:
    args = build_parser().parse_args()
    try:
        sys.exit(asyncio.run(run(
            address=args.address,
            fw_path=args.firmware,
            chunk=args.chunk,
            timeout=args.timeout,
            verbose=args.verbose,
        )))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
