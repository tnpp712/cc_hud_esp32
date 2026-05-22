# cc_hud host push CLI

`push_quota.py` sends a 9-byte BLE payload to the CC-HUD device, updating the
5-hour and 7-day quota bars shown on the hardware display.

---

## Install

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

---

## Usage

```bash
python push_quota.py \
  --5h-used 42 --5h-limit 100 \
  --7d-used 300 --7d-limit 1000 \
  --verbose
```

All four quota flags are required. Optional flags:

| Flag | Default | Description |
|------|---------|-------------|
| `--device-name` | `CC-HUD` | BLE advertisement name to scan for |
| `--timeout` | `10` | Scan timeout in seconds |
| `--verbose` | off | Print each step to stderr |

---

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Payload written successfully |
| 1 | Device not found within `--timeout` |
| 2 | Connection failed (BLE error) |
| 3 | GATT write failed |
| 130 | Interrupted (Ctrl-C) |

---

## Important: this CLI is transport-only

`push_quota.py` does not read quota data itself. You supply a **wrapper script**
that determines current usage from whatever source you prefer (`ccusage`,
`~/.claude/projects/**/*.jsonl`, an API call, etc.) and then invokes
`push_quota.py` with the numbers.

Example wrapper skeleton:

```bash
#!/usr/bin/env bash
# /path/to/wrapper.sh
set -euo pipefail

# Replace these with however you obtain quota numbers.
USED_5H=$(your_quota_source_5h_used)
LIM_5H=$(your_quota_source_5h_limit)
USED_7D=$(your_quota_source_7d_used)
LIM_7D=$(your_quota_source_7d_limit)

exec /path/to/.venv/bin/python /path/to/push_quota.py \
  --5h-used  "$USED_5H"  \
  --5h-limit "$LIM_5H"  \
  --7d-used  "$USED_7D"  \
  --7d-limit "$LIM_7D"
```

---

## Automate with launchd

1. Edit `com.cchud.push.plist`:
   - Replace `/REPLACE_ME/wrapper.sh` with the absolute path to your wrapper.
   - Replace `/usr/local/bin/python3` if your Python is elsewhere
     (e.g., use the venv Python: `/path/to/.venv/bin/python`).
     Because the wrapper calls `push_quota.py` via `exec`, the `ProgramArguments`
     entry should point at your shell and wrapper script, not directly at
     `push_quota.py`.

2. Install and load:

```bash
cp com.cchud.push.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.cchud.push.plist
```

The agent fires at load and then every 60 seconds. Logs land in
`/tmp/cchud.out.log` and `/tmp/cchud.err.log`.

To unload:

```bash
launchctl unload ~/Library/LaunchAgents/com.cchud.push.plist
```

---

## Troubleshooting

**Bluetooth permission denied on macOS**

macOS requires explicit Bluetooth access for each terminal application.
Go to **System Settings → Privacy & Security → Bluetooth** and enable your
terminal emulator (Terminal.app, iTerm2, etc.). Without this, `BleakScanner`
will silently find no devices.

**Device not found (exit code 1)**

- Confirm the CC-HUD device is powered on and advertising.
- Try increasing `--timeout` (e.g., `--timeout 20`).
- Run `--verbose` to see scan progress in real time.

**Write failed (exit code 3)**

- Ensure the firmware is running the correct service/characteristic UUIDs.
- The characteristic expects `WriteWithoutResponse`; check firmware flags if
  you modified them.
