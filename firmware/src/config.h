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
constexpr size_t  kQuotaPayloadLenV1     = 9;
constexpr size_t  kQuotaPayloadLenV2     = 17;
constexpr size_t  kQuotaPayloadLenV3Base = 27;   // mandatory fields before title
constexpr size_t  kQuotaPayloadLenV4Base = 8;    // mandatory fields before idle_status
constexpr uint8_t kQuotaMsgTypeV1        = 0x01;
constexpr uint8_t kQuotaMsgTypeV2        = 0x02;
constexpr uint8_t kQuotaMsgTypeV3        = 0x03;
constexpr uint8_t kQuotaMsgTypeV4        = 0x04;
// Out-of-band control message: 1 byte msg_type 0x05 + 1 byte command.
// cmd 0x00 = leave forced idle, cmd 0x01 = enter forced idle.
constexpr uint8_t kQuotaMsgTypeForceIdle = 0x05;

// Force-mood (debug / preview tool): msg_type 0x06 + 1 byte mood
// (0..4 → PetMood). 0xFF = release force, fall back to auto.
constexpr uint8_t kQuotaMsgTypeForceMood = 0x06;

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
// no v1/v2/v3 quota write. 30 minutes.
constexpr uint32_t kIdleThresholdMs = 30UL * 60UL * 1000UL;

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
