# cc_hud

A small desktop hardware HUD that mirrors Claude Code's rate-limit usage
(5-hour + 7-day) or API session cost on a 1.54" colour LCD, driven by an
ESP32-S3 and updated over Bluetooth Low Energy from a wrapper plugged into
your Claude Code statusline.

<p align="center">
  <img src="images/screen.jpg" alt="cc_hud running UI" width="42%" />
  &nbsp;&nbsp;
  <img src="images/screen-ota.jpg" alt="cc_hud OTA upgrade in progress" width="42%" />
</p>

*Left: live HUD showing the active plan title + 5H/7D usage with reset
countdown. Right: full-screen OTA progress takes over while a new firmware
streams in over BLE — the device reboots into the new image on completion.*

---

## Project at a glance

| Layer | What |
|---|---|
| Hardware | ESP32-S3-Nano (R8, 8 MB Flash + 8 MB OPI PSRAM) + 1.54" 240×240 IPS ST7789 + LiPo 503040 + TP4056 + MT3608 |
| Firmware | PlatformIO · Arduino-ESP32 · Adafruit_GFX/ST7789 · NimBLE-Arduino · NVS · `Update` library for BLE OTA |
| Host | Python 3.11+ · bleak · shell wrapper for Claude Code statusline · Python OTA uploader |
| Enclosure | One-piece 3D-printed case + drop-in back cover, parametric build123d sources, Bambu-compatible 3MF |
| BLE protocol | Custom GATT service, v3 payload supports both subscription mode (5H/7D + reset countdown + plan title) and API mode (cost + session duration) |
| OTA | Second GATT service streams firmware bytes into the inactive slot; arduino-esp32 `Update` library handles the slot switch + reboot |

---

## Bill of materials

| # | Part | Spec | Approx ¥ |
|---|---|---|---|
| 1 | ESP32-S3 main board | ESP32-S3R8 in Arduino Nano ESP32 footprint, 8 MB Flash | 35 |
| 2 | LCD module | 1.54" IPS ST7789 240×240, 4-wire SPI, 8-pin | 22 |
| 3 | LiPo battery | 503040 600 mAh (5 × 30 × 40 mm) with protection PCB | 15 |
| 4 | Charge IC module | TP4056 with USB-C input + DW01 protection | 3 |
| 5 | Boost converter | MT3608 (LiPo 3.7 V → 5 V, adjustable) | 4 |
| 6 | Slide switch | SS-12D00 (8.5 × 4 × 4 mm, side-mount) | 1 |
| 7 | Dupont / silicone wire | 30 cm of 0.5 mm² × 4 colours | 3 |
| 8 | Filament | ~25 g PETG (or PLA) for the case + cover | — |
| | | **Total** | **~¥83** |

---

## Wiring

### Display ↔ ESP32-S3 main board

The board exposes Arduino-Nano-style silkscreen pins; the GPIO numbers below
are the actual ESP32-S3 pads the firmware uses in `firmware/src/config.h`.

| LCD pad | Signal | → | Board pad | ESP32-S3 GPIO |
|---|---|---|---|---|
| GND | Ground | → | GND | — |
| VCC | +3.3 V (⚠ NOT 5 V) | → | 3V3 | — |
| SCL | SPI clock | → | D13 | GPIO48 |
| SDA | SPI MOSI | → | D11 | GPIO38 |
| RST | Reset | → | D8 | GPIO17 |
| DC | Data/Command | → | D7 | GPIO10 |
| SC (= CS) | Chip select | → | D10 | GPIO21 |
| BL | Backlight | → | D9 | GPIO18 |

If yours says `CS` instead of `SC`, same thing. MISO is unused (the LCD is
write-only) — leave it disconnected.

### Power supply chain (battery + charge + boost)

The TP4056 output is unregulated 3.0–4.2 V LiPo voltage, which is **out of
spec for the ESP32-S3's 3V3 rail**. The MT3608 boosts it back to a stable
5 V that the on-board regulator on the ESP32-S3 board can step down again.

```
       USB-C 5 V                                              ESP32-S3 board
       (TP4056 input)                                              VBUS  ◄──┐
            │                                                                │
            ▼                                                                │
       ┌────────────┐        ┌──────────┐       ┌──────────┐                 │
       │  TP4056    │  OUT+  │  Slide   │  IN+  │  MT3608  │  OUT+ (5.0 V) ──┘
       │  charger   │───────►│  switch  │──────►│  boost   │
       │  +protect  │  OUT-  │  SS-12D00│  IN-  │  3.7→5V  │  OUT-  ─────────►  GND
       └──┬─────┬───┘        └──────────┘       └──────────┘
          │ B+  │ B-
          ▼     ▼
       LiPo + / LiPo -   (503040 600 mAh)
```

Step-by-step solder list:

| Step | From | To | Wire colour |
|---|---|---|---|
| 1 | LiPo + | TP4056 **B+** | red |
| 2 | LiPo − | TP4056 **B−** | black |
| 3 | TP4056 **OUT+** | slide switch centre pin | red |
| 4 | Switch side pin (either) | MT3608 **IN+** | red |
| 5 | TP4056 **OUT−** | MT3608 **IN−** | black |
| 6 | MT3608 **OUT+** | ESP32 board **VBUS** | red |
| 7 | MT3608 **OUT−** | ESP32 board **GND** | black |

**⚠ Before step 6**: plug the LiPo, slide switch ON, multimeter the MT3608
output, rotate the small blue trimmer until it reads **5.0 V ± 0.1 V**.
Connecting an un-trimmed MT3608 can output 12 V and kill the ESP32-S3.

Charging works automatically: plug USB-C into the TP4056 module's input
(USB-C if your module has one, otherwise solder a USB-C breakout to IN+/IN-).
Red LED = charging, blue LED = charged.

**Do not** power the ESP32 board through its own USB-C *and* the boost
output at the same time — back-feeding. Either flip the slide switch OFF
while debugging over USB, or fit a dual-input OR'ing module (e.g. LM66100)
in a v2 build.

---

## Enclosure

Two STL/3MF parts, both parametric (`*.py` sources in repo root):

| File | What | Outer size |
|---|---|---|
| `cc_hud_case.3mf` | Main shell — front panel + screen window + **side USB-C cut-out** + 4 ESP32 standoffs + feet | 51 × 58 × 55 mm |
| `cc_hud_back_cover.3mf` | Drop-in lip-jointed back cover with pry-open notches | 51 × 58 × 5 mm |

Screen is **portrait** — long edge along Y. Active area 33 × 35 mm
(window cut into the front panel), screen module PCB 34 × 44 mm
sandwiched between the front panel and the ESP32 board.

USB-C exits the **right-hand side wall** (+X). The ESP32 board is laid
sideways inside the case (long edge 45 mm along X, short edge 32 mm
along Y) so its USB-C connector points naturally out of that side.

Internal layout (Z = depth, front panel at Z=0):

| Z range | Layer |
|---|---|
| 0 – 2 mm | Front panel |
| 2 – 6.2 mm | Screen module (active + PCB, 4.2 mm combined) |
| 6.2 – 7.6 mm | Wire gap behind the screen PCB (tight, 1.4 mm) |
| 7.6 – 9.2 mm | ESP32-S3 board PCB |
| 9.2 – ~14 mm | ESP32 components + USB-C body |
| ~14 – ~19 mm | LiPo cell (35 × 52 × ~5 mm) |
| ~19 – 53 mm | TP4056 + MT3608 + slide switch + cabling (~34 mm slack) |
| 53 – 55 mm | Back cover lip |

To regenerate after editing a parameter:

```bash
# Main case
PYTHONPATH=~/.claude/skills/cad/scripts python3 -m step \
    cc_hud_case.py --stl cc_hud_case.stl --skip-explorer
python3 ~/.claude/skills/bambu-3mf/scripts/stl_to_3mf.py \
    cc_hud_case.stl cc_hud_case.3mf

# Back cover (same recipe)
PYTHONPATH=~/.claude/skills/cad/scripts python3 -m step \
    cc_hud_back_cover.py --stl cc_hud_back_cover.stl --skip-explorer
python3 ~/.claude/skills/bambu-3mf/scripts/stl_to_3mf.py \
    cc_hud_back_cover.stl cc_hud_back_cover.3mf
```

Print orientation:

- **Main case**: front panel face down (screen window touching build plate).
  Open back faces up. No supports needed.
- **Back cover**: backplate face down, lip pointing up. No supports.

Recommended: PETG, 0.2 mm layer, 20 % gyroid infill, 3–4 walls.

---

## Software setup

### 1. Flash the firmware (one time, over USB)

```bash
cd firmware
pio run -t upload
```

If the chip is stuck in a boot loop and the USB CDC keeps disappearing,
use the 1200-baud rescue trick documented in
`docs/esp32s3-flash-recovery.md` (`stty -f /dev/cu.usbmodemXXXX 1200`
then `esptool ... --before no-reset write-flash ...`).

### 2. Set up the host environment

```bash
cd host
uv venv .venv
uv pip install -r requirements.txt
```

### 3. Find the device address (one time)

```bash
.venv/bin/python push_quota.py --discover --verbose --timeout 8
# Save the printed UUID (macOS) or MAC (Linux/Win) — used as --address below
```

### 4. Push some test data

```bash
.venv/bin/python push_quota.py \
    --address <YOUR_DEVICE_UUID> \
    --5h-used 45 --5h-limit 100 \
    --7d-used 230 --7d-limit 500 \
    --title "Plan Max (20x)" \
    --verbose
```

### 5. Wire into Claude Code statusline (automatic updates)

The `host/cchud-update.sh` wrapper is rate-limited (30 s by default) and
forks the BLE push into the background, so it's safe to call from the
statusline command (which fires every few seconds). Paste this near the
end of your `~/.claude/statusline.sh`:

```bash
CCHUD="$HOME/Desktop/work/cc_hud/host/cchud-update.sh"
if [ -x "$CCHUD" ]; then
    CCHUD_ADDR=<YOUR_DEVICE_UUID> \
    CCHUD_TITLE="Plan Max (20x)" \
    CCHUD_MODE=sub \
    "$CCHUD" "${FIVE_H_INT:-0}" 100 "${SEVEN_D_INT:-0}" 100 \
            "$CC_5H_RESET_S" "$CC_7D_RESET_S" \
            >/dev/null 2>&1 || true
fi
```

For API-mode users (no `rate_limits` in the statusline JSON), the wrapper
also accepts `CCHUD_MODE=api`, `CCHUD_COST_USD`, `CCHUD_DURATION_S`.

### 6. OTA upgrade (no more USB)

After the first USB flash, every subsequent firmware update goes over
Bluetooth:

```bash
.venv/bin/python ota.py \
    --address <YOUR_DEVICE_UUID> \
    --firmware ../firmware/.pio/build/esp32s3_nano/firmware.bin \
    --verbose
```

Takes ~3 minutes for a 600 KB image. The screen flips to a full-screen
"OTA UPDATE N%" + progress bar while the upload is happening, and the
device reboots into the new firmware on the END command.

---

## BLE protocol (v3 summary)

Service UUID: `12345678-aaaa-bbbb-cccc-1234567890ab`

| Characteristic | UUID | Properties | Purpose |
|---|---|---|---|
| Quota | `…0a1` | Write / WriteNoResp | Push the 27-bytes-plus-title payload |
| State | `…0a2` | Read / Notify | ACK / error feedback for each write |

OTA service: `12345678-aaaa-bbbb-cccc-1234567890bb`

| Characteristic | UUID | Properties | Purpose |
|---|---|---|---|
| Control | `…0b1` | Write | `0x00` START + size, `0x01` END, `0x02` ABORT |
| Data | `…0b2` | Write / WriteNoResp | Firmware byte stream |
| State | `…0b3` | Read / Notify | `READY` / `PROG N` / `OK` / `ERR …` |

⚠ **macOS quirk**: bleak's CoreBluetooth backend silently drops
`response=False` writes once an internal queue fills. Both `push_quota.py`
and `ota.py` force `response=True` for the data characteristics. Don't
revert that.

---

## File layout

```
cc_hud/
├── README.md                       this file
├── cc_hud_case.py                  parametric build123d source (main shell)
├── cc_hud_case.3mf                 Bambu-ready main shell
├── cc_hud_case.step                CAD-portable backup (main shell)
├── cc_hud_back_cover.py            parametric build123d source (back cover)
├── cc_hud_back_cover.3mf           Bambu-ready back cover
├── cc_hud_back_cover.step          CAD-portable backup (back cover)
├── firmware/                       PlatformIO project (Arduino-ESP32)
│   ├── platformio.ini
│   ├── partitions_ota_8mb.csv      dual-app OTA partition table
│   ├── sketch_hello/               minimal SPI smoke-test sketch
│   └── src/                        BLE server, OTA server, display, persistence, main
└── host/                           Python push/OTA tooling
    ├── push_quota.py               quota push CLI
    ├── ota.py                      OTA firmware uploader
    ├── cchud-update.sh             statusline wrapper (rate-limited)
    ├── com.cchud.push.plist        launchd template (optional alternative to statusline hook)
    └── requirements.txt
```

`docs/` is gitignored (Claude private workspace).

---

## Final assembly

Step order:

1. Print both 3MFs (PETG, ~25 g, ~1 h).
2. Solder the LCD ↔ ESP32 board per the table above.
3. Flash the firmware once over USB.
4. Set up the host venv + discover the device's BLE address.
5. Solder the power chain (LiPo → TP4056 → switch → MT3608 → ESP32),
   trim MT3608 to 5.0 V before connecting the ESP32.
6. Slide PCBs into the case from the back:
   - Screen PCB sits flush against the front panel, retained by the
     four corner hooks.
   - ESP32 board rests on the four standoffs, USB-C aligned with the
     top cut-out.
7. Tuck the battery + charge module + boost module + switch into the
   remaining cavity.
8. Snap on the back cover (lip joint, friction fit, two notches at the
   bottom for fingernail removal).
9. Wire the statusline hook (`~/.claude/statusline.sh`) — usage shows
   up on the LCD within 30 seconds.

---

## License

MIT
