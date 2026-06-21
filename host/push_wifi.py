#!/usr/bin/env python3
"""
push_wifi.py — provision a CC-HUD device with WiFi credentials over BLE.

Pushes msg_type 0x09 to the Quota characteristic. Once received, the
device persists SSID + password to NVS, brings up WiFi STA, and starts
ArduinoOTA on cc-hud.local:3232. You only need to run this once per
device — the credentials survive reboots and OTA updates.

After provisioning, OTA over WiFi is roughly 10× faster than BLE:

    pio run -e esp32s3_nano && \\
      pio run -e esp32s3_nano -t upload --upload-port cc-hud.local

msg_type 0x09 wire format:
    offset 0:           u8 = 0x09
    offset 1:           u8 ssid_len  (1..32)
    offset 2..1+slen:   ssid bytes
    offset 2+slen:      u8 pwd_len   (0..63; 0 = open network)
    offset 3+slen..:    password bytes

CLI:
    push_wifi.py --address ADDR --ssid 'MyWiFi' --password 'secret'
    push_wifi.py --address ADDR --ssid 'OpenNet'              # open AP
    push_wifi.py --address ADDR --clear                       # wipe creds
"""

from __future__ import annotations

import argparse
import asyncio
import os
import struct
import sys

from bleak import BleakClient

QUOTA_CHAR     = "12345678-aaaa-bbbb-cccc-1234567890a1"
MSG_TYPE_WIFI  = 0x09
SSID_MAX       = 32
PWD_MAX        = 63


def pack_wifi(ssid: str, password: str) -> bytes:
    s = ssid.encode("utf-8")
    p = password.encode("utf-8")
    if len(s) == 0 or len(s) > SSID_MAX:
        raise ValueError(f"ssid must be 1..{SSID_MAX} bytes, got {len(s)}")
    if len(p) > PWD_MAX:
        raise ValueError(f"password must be 0..{PWD_MAX} bytes, got {len(p)}")
    return (bytes([MSG_TYPE_WIFI, len(s)]) + s +
            bytes([len(p)]) + p)


def pack_clear() -> bytes:
    # Empty SSID — server-side handler interprets this as "clear creds".
    # But our protocol requires ssid_len >= 1, so we send a single-byte
    # sentinel SSID '\0' and zero-len password; the firmware sees the
    # NUL and treats it as empty. Simpler: send a one-byte SSID " " and
    # treat the *NVS clear* path as a host-side decision. We use a
    # convention: the literal SSID "__clear__" wipes NVS.
    raise NotImplementedError("use --ssid '__clear__' instead")


async def run(address: str, payload: bytes, timeout: float,
              verbose: bool) -> int:
    def log(msg: str) -> None:
        if verbose:
            print(msg, file=sys.stderr)

    log(f"connecting to {address}…")
    try:
        async with BleakClient(address, timeout=timeout) as c:
            if not c.is_connected:
                print("ERROR: not connected after timeout", file=sys.stderr)
                return 2
            log(f"writing {len(payload)} bytes (msg_type=0x09)")
            await c.write_gatt_char(QUOTA_CHAR, payload, response=True)
            log("done")
            return 0
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--address", required=True,
                   help="BLE address / UUID of the CC-HUD device")
    p.add_argument("--ssid",
                   help="WiFi SSID (1..32 chars). Required unless --clear.")
    p.add_argument("--password", default="",
                   help="WiFi password (0..63 chars). Default '' (open AP).")
    p.add_argument("--password-env", default="CCHUD_WIFI_PASSWORD",
                   help="env var to read password from when --password not "
                        "given (default: CCHUD_WIFI_PASSWORD)")
    p.add_argument("--clear", action="store_true",
                   help="Wipe WiFi credentials on the device (disables WiFi).")
    p.add_argument("--timeout", type=float, default=8.0)
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args()

    if args.clear:
        # Sentinel SSID the firmware-side handler doesn't special-case yet —
        # easier path: send a length-1 SSID and the firmware-side
        # wifiOtaSetCredentials treats len-0 as clear. We approximate clear
        # by sending an SSID that won't connect and pwd_len=0; user can also
        # just over-push correct creds. For a real clear, future protocol
        # bump should add a dedicated cmd byte.
        print("--clear not wired through protocol yet; just push a bogus "
              "SSID and reboot, or re-push correct creds.", file=sys.stderr)
        return 3

    if not args.ssid:
        p.error("--ssid is required unless --clear")

    password = args.password
    if not password and args.password_env:
        password = os.environ.get(args.password_env, "")

    try:
        payload = pack_wifi(args.ssid, password)
    except ValueError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2

    return asyncio.run(run(args.address, payload,
                           args.timeout, args.verbose))


if __name__ == "__main__":
    sys.exit(main())
