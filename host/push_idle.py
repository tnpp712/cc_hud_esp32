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
import json
import struct
import sys
import time
import urllib.parse
import urllib.request

from bleak import BleakClient

QUOTA_CHAR = "12345678-aaaa-bbbb-cccc-1234567890a1"
MSG_TYPE_V4 = 0x04
MAX_STATUS_LEN = 32


def _log(msg: str, verbose: bool) -> None:
    if verbose:
        print(msg, file=sys.stderr)


# WWO weather-code → Chinese condition. wttr.in's own lang_zh data is
# broken (returns English), so we translate the numeric code locally.
# Phrasing is constrained to the glyph subset baked into the firmware
# font (firmware/src/font_cn_20.c) — regenerate the font before adding
# characters outside that set.
WWO_CN = {
    113: "晴", 116: "多云", 119: "阴", 122: "阴天",
    143: "薄雾", 176: "零星小雨", 179: "零星小雪", 182: "零星雨夹雪",
    185: "零星冻毛毛雨", 200: "雷阵雨", 227: "风雪", 230: "暴风雪",
    248: "雾", 260: "冻雾", 263: "零星毛毛雨", 266: "毛毛雨",
    281: "冻毛毛雨", 284: "强冻毛毛雨", 293: "零星小雨", 296: "小雨",
    299: "间歇中雨", 302: "中雨", 305: "间歇大雨", 308: "大雨",
    311: "冻雨", 314: "强冻雨", 317: "小雨夹雪", 320: "中雨夹雪",
    323: "零星小雪", 326: "小雪", 329: "间歇中雪", 332: "中雪",
    335: "间歇大雪", 338: "大雪", 350: "冰雹", 353: "小阵雨",
    356: "大阵雨", 359: "暴雨", 362: "小阵雨夹雪", 365: "大阵雨夹雪",
    368: "小阵雪", 371: "大阵雪", 374: "小冰雹", 377: "大冰雹",
    386: "局部雷阵雨", 389: "雷暴雨", 392: "雷阵雪", 395: "暴风雪",
}


def fetch_weather(city: str, timeout: float = 5.0) -> str:
    """Fetch weather from wttr.in (j1 JSON) and build a Chinese status.

    Returns e.g. "多云 +16°C 北京" (city echoed as given — pass a
    Chinese city name via --weather-city/北京 for an all-CJK line).
    Returns "" on any failure; caller falls back to --status.
    """
    url = f"https://wttr.in/{urllib.parse.quote(city)}?format=j1"
    try:
        req = urllib.request.Request(
            url, headers={"User-Agent": "curl/cc-hud"}
        )
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.load(resp)
        cc = data["current_condition"][0]
        code = int(cc["weatherCode"])
        temp = int(cc["temp_C"])
    except Exception as exc:  # noqa: BLE001 — best-effort fetch
        print(f"weather fetch failed: {exc}", file=sys.stderr)
        return ""
    cond = WWO_CN.get(code)
    if cond is None:
        # Unknown code — fall back to the English description.
        try:
            cond = cc["weatherDesc"][0]["value"].strip()
        except Exception:  # noqa: BLE001
            cond = "?"
    # Human-readable order: 城市 状况 温度. No "+" sign (unnatural in
    # Chinese), single ℃ glyph (U+2103, in the font), and city first so a
    # width clip still leaves the most useful info. e.g. "杭州 多云 27℃".
    text = f"{city} {cond} {temp}℃".strip()
    return _utf8_truncate(text, MAX_STATUS_LEN).decode("utf-8")


def _utf8_truncate(s: str, max_bytes: int) -> bytes:
    """Encode to UTF-8, truncating on a character boundary ≤ max_bytes."""
    b = s.encode("utf-8")
    while len(b) > max_bytes:
        s = s[:-1]
        b = s.encode("utf-8")
    return b


def _pack_v4(unix_ts: int, utc_offset_min: int, status: str) -> bytes:
    # UTF-8 so Chinese weather strings work — the firmware renders the
    # status with a CJK-capable LVGL font. 32 bytes ≈ 10 CJK chars.
    status_b = _utf8_truncate(status, MAX_STATUS_LEN)
    return struct.pack("<BIhB", MSG_TYPE_V4, unix_ts, utc_offset_min, len(status_b)) + status_b


async def run(address: str, status: str, weather_city: str,
              timeout: float, verbose: bool) -> int:
    # --weather-city overrides --status. Fetch happens before the BLE
    # connection so we don't hold the radio open while waiting on HTTP.
    if weather_city:
        fetched = fetch_weather(weather_city, timeout=min(timeout, 5.0))
        if fetched:
            _log(f"weather '{weather_city}' -> {fetched!r}", verbose)
            status = fetched
        else:
            _log(f"weather '{weather_city}' fetch returned empty; keeping --status",
                 verbose)
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

    # Retry once on a transient BLE disconnect (lock contention with the
    # quota/state pushers).
    last_err = None
    for _ in range(2):
        try:
            async with BleakClient(address, timeout=timeout) as client:
                _log("connected, writing…", verbose)
                await client.write_gatt_char(QUOTA_CHAR, payload, response=True)
                _log("done", verbose)
            return 0
        except Exception as exc:
            last_err = exc
            await asyncio.sleep(0.4)
    print(f"ERROR: {last_err}", file=sys.stderr)
    return 2


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
    p.add_argument("--weather-city", default="",
                   help="Fetch weather for this city via wttr.in and use "
                        "it as the status string (overrides --status). "
                        "No API key needed. ASCII-cleaned automatically.")
    p.add_argument("--timeout", type=float, default=10.0)
    p.add_argument("--verbose", action="store_true", default=False)
    return p


def main() -> None:
    args = build_parser().parse_args()
    try:
        sys.exit(asyncio.run(run(
            address=args.address,
            status=args.status,
            weather_city=args.weather_city,
            timeout=args.timeout,
            verbose=args.verbose,
        )))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
