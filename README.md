# cc_hud

A small hardware display for Claude Code usage quotas. Push 5-hour and 7-day usage from your machine over BLE to an ESP32-S3 with a 1.54" color LCD.

## Hardware

- ESP32-S3-Nano (Arduino Nano ESP32 form factor)
- ST7789 1.54" 240x240 4-Wire SPI LCD

## Architecture

```
[host script]  --BLE GATT-->  [ESP32-S3]  --SPI-->  [ST7789 LCD]
```

- `firmware/` — Arduino-ESP32 firmware, BLE GATT server, ST7789 renderer
- `host/` — Python CLI that pushes quota data via BLE

## Quick start

### 1. Flash firmware

```bash
cd firmware
pio run -t upload
```

### 2. Push quota from host

```bash
cd host
pip install -r requirements.txt
python push_quota.py --5h-used 45 --5h-limit 100 --7d-used 230 --7d-limit 500
```

The push script can be wrapped in `launchd` (macOS) to poll every 60 seconds — see `host/README.md`.

## License

MIT
