# cc_hud host push CLI

`push_quota.py` sends a 9-byte BLE payload to the CC-HUD device, updating the
5-hour and 7-day quota bars shown on the hardware display.

`cchud-update.sh` is a rate-limited fire-and-forget wrapper meant for
high-frequency hooks (Claude Code statusline, shell prompts, etc.).

---

## Install

```bash
# Recommended (uv):
uv venv .venv
uv pip install -r requirements.txt

# Or plain Python:
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

---

## First-time setup: discover your device

macOS hides BLE MAC addresses, so each peripheral is identified by a
CoreBluetooth UUID instead. Linux and Windows show the real MAC. Find yours:

```bash
.venv/bin/python push_quota.py --discover --verbose --timeout 8
```

Example output:

```
ADDRESS                                   NAME              RSSI
03265204-7126-B6FE-6F17-6E9621CEA97F      CC-HUD            -37 dBm
```

Save that address — use it with `--address` (push_quota.py) or via
`CCHUD_ADDR` (cchud-update.sh) so subsequent pushes skip the scan.

---

## Usage — single push

```bash
.venv/bin/python push_quota.py \
  --address 03265204-7126-B6FE-6F17-6E9621CEA97F \
  --5h-used 42 --5h-limit 100 \
  --7d-used 300 --7d-limit 1000 \
  --verbose
```

| Flag | Default | Description |
|------|---------|-------------|
| `--address` | (none) | BLE address/UUID. Skips scan when provided. |
| `--device-name` | `CC-HUD` | Advertisement name (used by `--discover` and the scan fallback). |
| `--discover` | off | Scan and print all matching devices, then exit. |
| `--timeout` | `10` | Scan/connect timeout in seconds. |
| `--verbose` | off | Print each step to stderr. |

Exit codes: `0` ok · `1` device not found · `2` connect failed ·
`3` write failed · `130` Ctrl-C.

> ⚠️ The Quota characteristic is written with `response=True` (Write With
> Response). bleak's CoreBluetooth backend silently drops `response=False`
> packets, so the firmware never sees them. This is enforced internally.

---

## Usage — statusline integration (recommended)

`cchud-update.sh` is the wrapper to call from a frequently-firing hook like
Claude Code's `~/.claude/statusline.sh`. It:

- Returns in **<50 ms** (no BLE on the foreground path).
- Skips silently if it pushed within the last 30 seconds
  (override via `CCHUD_RATE_LIMIT`).
- Forks the actual BLE write into the background, logged to
  `/tmp/cchud-push.log`.

### 1. Set the device address

Either edit the `CCHUD_ADDR` default inside `cchud-update.sh`, or export it
from the script that invokes it:

```bash
export CCHUD_ADDR='03265204-7126-B6FE-6F17-6E9621CEA97F'
```

### 2. Hook into your `statusline.sh`

Append near the end of your `~/.claude/statusline.sh` (or whatever it's called
in your setup):

```bash
# --- CC-HUD: push current quota over BLE (fire-and-forget) -----------------
# Replace H5_USED / H5_LIMIT / D7_USED / D7_LIMIT with whichever variables
# your statusline script already computes from the Claude Code session.
/Users/firshme/Desktop/work/cc_hud/host/cchud-update.sh \
  "${H5_USED:-0}" "${H5_LIMIT:-0}" \
  "${D7_USED:-0}" "${D7_LIMIT:-0}" \
  >/dev/null 2>&1 || true
# --------------------------------------------------------------------------
```

The `|| true` guard means a transient BLE failure never breaks your
statusline output.

### 3. Tail the background log

```bash
tail -f /tmp/cchud-push.log
```

### Environment overrides

| Variable | Default | Meaning |
|---|---|---|
| `CCHUD_ADDR` | (hardcoded UUID) | BLE address/UUID of the device. |
| `CCHUD_RATE_LIMIT` | `30` | Minimum seconds between actual pushes. |
| `CCHUD_LOG` | `/tmp/cchud-push.log` | Log path for the background bleak process. |
| `CCHUD_TIMEOUT` | `8` | Scan/connect timeout in seconds. |

---

## Important: this CLI is transport-only

Neither `push_quota.py` nor `cchud-update.sh` figures out the quota numbers
themselves. You supply them — from `ccusage`, your own jsonl parser, the
Claude Code session statusline, or wherever — and pass them as four
integers.

---

## Automate with launchd (alternative to statusline hook)

For a fixed 60-second poll independent of statusline activity:

1. Edit `com.cchud.push.plist`:
   - Replace `/REPLACE_ME/wrapper.sh` with an absolute path to your wrapper.
   - Confirm `/usr/local/bin/python3` is correct (or use the venv Python:
     `/path/to/cc_hud/host/.venv/bin/python`).

2. Install and load:

   ```bash
   cp com.cchud.push.plist ~/Library/LaunchAgents/
   launchctl load ~/Library/LaunchAgents/com.cchud.push.plist
   ```

Logs in `/tmp/cchud.out.log` / `/tmp/cchud.err.log`. Unload with
`launchctl unload …`.

---

## Troubleshooting

**`Bluetooth device is turned off`**

Either macOS Bluetooth is off, or the terminal running the script doesn't
have Bluetooth permission. Grant it in **System Settings → Privacy &
Security → Bluetooth**.

**Device not found (exit code 1)**

- Confirm the CC-HUD device is powered on and advertising (the panel should
  show "CC HUD" header + "waiting for data...").
- Try `--timeout 20`.
- Re-run `--discover --verbose` to see if scanning works at all.

**Write succeeds but screen doesn't update**

If you copy old code that uses `response=False`, the BLE packet is silently
dropped on macOS + NimBLE. The current `push_quota.py` forces `response=True`
internally — make sure you're not bypassing it.

**Statusline pushes nothing**

- Check `/tmp/cchud-push.log` — bleak errors land there.
- Check `/tmp/cchud-last-push.ts` exists and is recent (rate-limit check).
- Run `cchud-update.sh 1 100 1 100` by hand and watch the log.
