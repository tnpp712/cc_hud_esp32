// config.h
// Central definitions for cc_hud firmware: pin map, BLE UUIDs, colors, layout
// constants, NVS keys, thresholds and protocol-related sizes.
//
// Hardware: ESP32-S3-Nano (Arduino Nano ESP32 pinout) + ST7789 1.54" 240x240
// LCD on 4-wire SPI. Pins listed here mirror the wiring documented in the
// project root README and CLAUDE.md.

#pragma once

#include <Arduino.h>

namespace cc_hud {

// ---------------------------------------------------------------------------
// Hardware pin map (board pin -> ESP32 GPIO). Already configured for TFT_eSPI
// via build_flags in platformio.ini; the constants below are repeated for
// project-side use (e.g. backlight control or future expansion).
// ---------------------------------------------------------------------------
constexpr uint8_t kPinLcdMosi = 38;  // D11
constexpr uint8_t kPinLcdSclk = 48;  // D13
constexpr uint8_t kPinLcdCs   = 21;  // D10
constexpr uint8_t kPinLcdDc   = 10;  // D7
constexpr uint8_t kPinLcdRst  = 17;  // D8
constexpr uint8_t kPinLcdBl   = 18;  // D9

// ---------------------------------------------------------------------------
// WS2812B status LED ring (8 pixels), powered from the 3.7V lithium
// directly (before boost), DIN -> GPIO via 470Ω, with a 470µF cap across
// VCC/GND. Common-ground with the ESP32.
//
// kPinLedRing must be the *GPIO number*, not the silkscreen "Dx" label.
// On the genuine Arduino Nano ESP32 mapping, silk "D5" = GPIO8; on some
// cheap clones silk "D5" = GPIO5. If the ring doesn't light up after a
// fresh OTA, swap this between 5 and 8 to match your board.
// ---------------------------------------------------------------------------
constexpr uint8_t  kPinLedRing        = 5;   // silk "D2" on Arduino Nano ESP32 standard mapping

// ---------------------------------------------------------------------------
// Optional add-on hardware (firmware ships ready; safe when unwired).
//
// Battery voltage sense (GPIO1 = ADC1_CH0). Wire a 2:1 divider:
//   BAT+ ── 100kΩ ──┬── GPIO1
//                   └── 100kΩ ── GND
// So the ADC sees half the battery voltage (4.2V → 2.1V, within range).
// When the pin is left floating (not yet wired) the reading is implausible
// and the firmware treats it as "no battery sensor" — no false warnings.
//
// Push button (GPIO2 → button → GND). Uses the internal pull-up, so an
// unwired pin reads HIGH (released). Short press = manual page advance,
// long press = toggle manual dim.
// ---------------------------------------------------------------------------
constexpr uint8_t  kPinBatterySense   = 1;    // ADC1_CH0
constexpr uint8_t  kPinButton         = 2;
constexpr uint16_t kBatDividerRatioX10 = 20;  // 2.0× (×10 to stay integer)
constexpr uint16_t kBatFullMv         = 4200; // Li-ion 100%
constexpr uint16_t kBatEmptyMv        = 3000; // Li-ion 0%
constexpr uint16_t kBatLowMv          = 3450; // low-battery warning threshold
constexpr uint16_t kBatPlausibleMinMv = 2500; // below → assume no sensor wired
constexpr uint16_t kBatPlausibleMaxMv = 4500; // above → assume no sensor wired
constexpr uint32_t kButtonLongPressMs = 800;  // ≥ this = long press
constexpr uint32_t kButtonDebounceMs  = 30;
constexpr uint16_t kLedRingCount      = 24;   // WS2812B 24-position ring (per BOM)
// Brightness 0..255. 24 LEDs lit together (gauge / red full-ring) get glary
// fast, so keep this low. Day ~16, night dims to kLedRingBrightnessNight.
constexpr uint8_t  kLedRingBrightness      = 10;  // daytime
constexpr uint8_t  kLedRingBrightnessNight = 3;   // 23:00–07:00 auto-dim
constexpr uint32_t kLedChaseStepMs    = 90;   // 90ms × 24 ≈ 2.2s rotation (calm)
constexpr uint8_t  kLedCometLen       = 7;    // lit pixels in the comet tail (of 24)
constexpr uint32_t kLedBlinkHalfMs    = 500;  // 500ms on / 500ms off

// ---------------------------------------------------------------------------
// Firmware identity + NTP time. kFirmwareVersion is shown on the web
// dashboard for support. NTP auto-syncs the clock whenever WiFi is up, so
// the clock survives a power loss without a manual BLE re-calibration.
// kDefaultTzMin is the timezone used until a BLE idle-push provides one
// (480 = UTC+8, China). NVS-persisted tz overrides it.
// ---------------------------------------------------------------------------
constexpr const char* kFirmwareVersion = "2026.06.21";
constexpr int16_t     kDefaultTzMin    = 480;   // UTC+8
constexpr const char* kNtpServer1      = "ntp.aliyun.com";
constexpr const char* kNtpServer2      = "pool.ntp.org";

// ---------------------------------------------------------------------------
// BLE protocol identifiers. Service and characteristic UUIDs must match the
// host-side bridge exactly.
// ---------------------------------------------------------------------------
constexpr const char* kBleDeviceName        = "CC-HUD";
constexpr const char* kBleServiceUuid       = "12345678-aaaa-bbbb-cccc-1234567890ab";
constexpr const char* kBleQuotaCharUuid     = "12345678-aaaa-bbbb-cccc-1234567890a1";
constexpr const char* kBleStateCharUuid     = "12345678-aaaa-bbbb-cccc-1234567890a2";

// Quota payload framing — four versions are accepted by the firmware.
//   v1 (0x01, 9 bytes):  used+limit for 5h and 7d only.
//   v2 (0x02, 17 bytes): v1 + two u32 LE "seconds-until-reset" counters.
//   v3 (0x03, 27+title_len bytes): v2 + mode byte + cost + duration + title.
//     The firmware decrements reset_in_s in real time via millis() so the
//     countdown stays accurate between pushes. Mode 0 = subscription (show
//     5h/7d), mode 1 = api (show cost + duration).
//   v4 (0x04, 8+status_len bytes): idle-mode payload — pushes the wall clock
//     and a free-form status string. The firmware keeps the clock alive
//     between pushes via millis(), and auto-switches to the idle display
//     after ~30 minutes without a v1/v2/v3 quota write.
//   v5 (0x08, 28+title_len bytes): v3 layout + a 1-byte context-window
//     usage percent (0..100) inserted right before title_len. Lets the HUD
//     show how full the active session's context is. Fully backward
//     compatible — the firmware still accepts v1..v4.
//   v6 (0x0A, 36+title_len bytes): v5 layout + two u32 LE line counters
//     (lines_added, lines_removed) inserted right after ctx_pct, before
//     title_len. Feeds the session-stats screen (stage 2). Fully backward
//     compatible — the firmware still accepts v1..v5.
constexpr size_t  kQuotaPayloadLenV1     = 9;
constexpr size_t  kQuotaPayloadLenV2     = 17;
constexpr size_t  kQuotaPayloadLenV3Base = 27;   // mandatory fields before title
constexpr size_t  kQuotaPayloadLenV4Base = 8;    // mandatory fields before idle_status
constexpr size_t  kQuotaPayloadLenV5Base = 28;   // v3 base + 1 ctx_pct byte
constexpr size_t  kQuotaPayloadLenV6Base = 36;   // v5 base + 2× u32 line counters
constexpr uint8_t kQuotaMsgTypeV1        = 0x01;
constexpr uint8_t kQuotaMsgTypeV2        = 0x02;
constexpr uint8_t kQuotaMsgTypeV3        = 0x03;
constexpr uint8_t kQuotaMsgTypeV4        = 0x04;
constexpr uint8_t kQuotaMsgTypeV5        = 0x08;
constexpr uint8_t kQuotaMsgTypeV6        = 0x0A;
// Out-of-band control message: 1 byte msg_type 0x05 + 1 byte command.
// cmd 0x00 = leave forced idle, cmd 0x01 = enter forced idle.
constexpr uint8_t kQuotaMsgTypeForceIdle = 0x05;

// Force-mood (debug / preview tool): msg_type 0x06 + 1 byte mood
// (0..4 → PetMood). 0xFF = release force, fall back to auto.
constexpr uint8_t kQuotaMsgTypeForceMood = 0x06;

// WiFi credentials push (msg_type 0x09, ≥3 bytes):
//   offset 0: u8 msg_type = 0x09
//   offset 1: u8 ssid_len (1..32)
//   offset 2..1+ssid_len: ssid bytes
//   offset 2+ssid_len:    u8 pwd_len (0..63; 0 = open network)
//   offset 3+ssid_len..:  pwd bytes
// Pushed once via BLE to provision the device for WiFi OTA. Stored in NVS.
constexpr uint8_t kQuotaMsgTypeWifi      = 0x09;
constexpr uint8_t kQuotaMsgTypeV7Tlv    = 0x0B;   // v7 统一 TLV 帧
constexpr size_t  kWifiSsidMaxLen        = 32;
constexpr size_t  kWifiPwdMaxLen         = 63;

// App-state push (msg_type 0x07, ≥3 bytes):
//   offset 0: u8 msg_type = 0x07
//   offset 1: u8 state (see AppState below)
//   offset 2: u8 detail_len (0..15)
//   offset 3..2+detail_len: ASCII detail (tool name when state == Tool)
//   offset 3+detail_len:   u8 total_sessions (OPTIONAL, stage 3)
//   offset 4+detail_len:   u8 busy_sessions  (OPTIONAL, stage 3)
// The two session-count bytes are optional for backward compatibility:
// a host that only sends the first 3+detail_len bytes still parses. Both
// default to 0 ("unknown / single session") when absent.
// Pushed by Claude Code hooks via host wrapper. Each push completely
// replaces the previous app state.
constexpr uint8_t kQuotaMsgTypeState     = 0x07;
constexpr size_t  kAppStateDetailMaxLen  = 15;

enum AppState : int8_t {
    kAppStateIdle     = 0,  // Stop hook — Claude finished its turn
    kAppStateThinking = 1,  // UserPromptSubmit / PostToolUse
    kAppStateTool     = 2,  // PreToolUse — detail = tool name
    kAppStateWaiting  = 3,  // Notification — waiting on permission / no resp
    kAppStateDone     = 4,  // one-shot "turn finished" → green done-pulse, else idle
    kAppStateUnset    = -1, // boot-time sentinel
};

// Pet mood, derived from the highest of {5h%, 7d%} usage. Each mood
// has its own sprite + walking cadence, so the cat actually feels the
// load you're putting on Claude Code.
enum PetMood : int8_t {
    kPetMoodHappy       = 0,   //  <30% — chill, normal walk
    kPetMoodCalm        = 1,   //  30-60% — steady
    kPetMoodTired       = 2,   //  60-80% — slowing down
    kPetMoodStressed    = 3,   //  80-95% — fast & jittery
    kPetMoodOverwhelmed = 4,   //  >=95% — stops, cries
    kPetMoodAuto        = -1,  //  sentinel: derive from quota
};
constexpr int8_t kPetMoodCount = 5;

// Idle-mode trigger: switch to the clock screen after this many ms of
// no v1/v2/v3 quota write. 8 hours — long enough that normal "coffee
// break" pauses in a Claude Code session don't yank the HUD to the
// clock screen. Force-idle (msg 0x05 cmd 1) still switches immediately.
constexpr uint32_t kIdleThresholdMs = 8UL * 60UL * 60UL * 1000UL;

// Page carousel + activity-lock state machine (stage 2/3). When Claude is
// idle (app_state idle, not AFK) the HUD auto-rotates between the quota
// page and the session-stats page every kCarouselMs. When Claude is busy
// (tool/thinking/waiting) the UI locks to the activity (tool) page; to
// avoid flicker from sub-second tool↔thinking↔idle churn, the lock sticks
// for kBusyStickMs after the last busy signal before falling back to the
// carousel.
constexpr uint32_t kCarouselMs  = 6000;   // 6 s per page in idle rotation
constexpr uint32_t kBusyStickMs = 3500;   // hold the tool page 3.5 s post-busy

// Power saver: after this long with no Claude activity (app_state idle),
// dim the panel + LED ring just like night mode. Any activity restores
// full brightness instantly. Saves power + glare during long idle stretches.
constexpr uint32_t kIdleDimMs   = 180000;  // 3 min  → dim panel + LED
constexpr uint32_t kLedOffMs    = 600000;  // 10 min → extinguish the LED ring
// (saves the ~20mA WS2812B quiescent during long idle; restored on activity).
// WiFi stays ON — power-source auto-detect isn't reliable on this board's USB.

// Idle status string max length (same envelope as title).
constexpr size_t  kIdleStatusMaxLen = 32;
// Mode flags (v3+).
constexpr uint8_t kModeSubscription = 0x00;
constexpr uint8_t kModeApi          = 0x01;
// Title constraints (v3+).
constexpr size_t  kTitleMaxLen = 32;     // title_len byte caps payload growth
constexpr size_t  kTitleBufLen = 33;     // 32 chars + trailing null
// Back-compat aliases used by older code paths.
constexpr size_t  kQuotaPayloadLen = kQuotaPayloadLenV1;
constexpr uint8_t kQuotaMsgType    = kQuotaMsgTypeV1;

// State notifications (short ASCII strings).
constexpr const char* kStateOk      = "OK";
constexpr const char* kStateErrLen  = "ERR len";
constexpr const char* kStateErrType = "ERR type";

// ---------------------------------------------------------------------------
// NVS keys (Preferences namespace "cc_hud"). Names are <=15 chars each.
// ---------------------------------------------------------------------------
constexpr const char* kNvsNamespace  = "cc_hud";
constexpr const char* kNvsKey5hUsed  = "5h_used";
constexpr const char* kNvsKey5hLim   = "5h_lim";
constexpr const char* kNvsKey7dUsed  = "7d_used";
constexpr const char* kNvsKey7dLim   = "7d_lim";
constexpr const char* kNvsKeyTs      = "ts";
constexpr const char* kNvsKey5hReset = "5h_reset";
constexpr const char* kNvsKey7dReset = "7d_reset";
constexpr const char* kNvsKeyMode    = "mode";
constexpr const char* kNvsKeyCostU   = "cost_uusd";   // cost in micro-USD
constexpr const char* kNvsKeyDurS    = "dur_s";
constexpr const char* kNvsKeyTitle   = "title";
constexpr const char* kNvsKey5hAlert = "5h_alerted"; // bool: already flashed?
constexpr const char* kNvsKey7dAlert = "7d_alerted";
constexpr const char* kNvsKeyUnixTs  = "unix_ts";    // last received UTC Unix time
constexpr const char* kNvsKeyTzOff   = "tz_off";     // UTC offset in minutes (signed)
constexpr const char* kNvsKeyTsCap   = "ts_cap";     // millis() at capture
constexpr const char* kNvsKeyIdleStr = "idle_str";   // idle status string

// ---------------------------------------------------------------------------
// Display geometry and color palette. 16-bit RGB565 values.
// ---------------------------------------------------------------------------
constexpr int16_t kScreenWidth  = 240;
constexpr int16_t kScreenHeight = 240;

constexpr int16_t kHeaderY      = 0;
constexpr int16_t kHeaderH      = 28;
constexpr int16_t k5hRowY       = 36;
constexpr int16_t k5hRowH       = 72;   // 36..108
constexpr int16_t k7dRowY       = 120;
constexpr int16_t k7dRowH       = 72;   // 120..192
constexpr int16_t kFooterY      = 210;
constexpr int16_t kFooterH      = 26;   // 210..236

constexpr int16_t kRowMargin    = 8;    // left/right text inset
constexpr int16_t kBarHeight    = 14;
constexpr int16_t kBarMarginX   = 8;    // inset for progress bar
constexpr int16_t kHeaderDotR   = 8;    // status dot radius
constexpr int16_t kHeaderDotPad = 14;

constexpr uint16_t kColorBg       = 0x0000;  // black
constexpr uint16_t kColorFg       = 0xFFFF;  // white
constexpr uint16_t kColorMuted    = 0x7BEF;  // light grey
constexpr uint16_t kColorGreen    = 0x07E0;
constexpr uint16_t kColorYellow   = 0xFFE0;
constexpr uint16_t kColorRed      = 0xF800;
constexpr uint16_t kColorBarTrack = 0x2104;  // dark grey track
constexpr uint16_t kColorDotIdle  = 0x52AA;  // grey when advertising
constexpr uint16_t kColorDotLink  = 0x07E0;  // green when connected

// Thresholds for color tiers, in percent.
constexpr uint8_t kThreshGreenMax  = 60;   // 0..60 inclusive -> green
constexpr uint8_t kThreshYellowMax = 84;   // 61..84         -> yellow ( >=85 red )

// One-shot threshold-crossing alert. When 5H or 7D usage first crosses
// kAlertThresholdPct, the firmware flashes the screen red for a few cycles.
// The "alerted" flag is persisted in NVS so we don't re-flash every push;
// it resets when the percentage drops back below the threshold.
constexpr uint8_t  kAlertThresholdPct    = 95;
constexpr uint8_t  kAlertFlashCycles     = 5;     // on+off pairs
constexpr uint32_t kAlertFlashIntervalMs = 500;   // half-period

// Staleness threshold (minutes) before footer turns red.
constexpr uint32_t kStaleThresholdMin = 5;

// Footer / loop cadence.
constexpr uint32_t kFooterRefreshMs = 5000;   // 5 s tick for "updated Xm ago"

// Sentinel for "no quota ever received" (e.g. on first boot).
constexpr uint64_t kTsUnset = 0;

}  // namespace cc_hud
