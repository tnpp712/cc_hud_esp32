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

// Quota payload framing.
constexpr size_t  kQuotaPayloadLen = 9;       // 1 + 4 * 2 little-endian
constexpr uint8_t kQuotaMsgType    = 0x01;    // structured quota message

// State notifications (short ASCII strings).
constexpr const char* kStateOk      = "OK";
constexpr const char* kStateErrLen  = "ERR len";
constexpr const char* kStateErrType = "ERR type";

// ---------------------------------------------------------------------------
// NVS keys (Preferences namespace "cc_hud"). Names are <=15 chars each.
// ---------------------------------------------------------------------------
constexpr const char* kNvsNamespace = "cc_hud";
constexpr const char* kNvsKey5hUsed = "5h_used";
constexpr const char* kNvsKey5hLim  = "5h_lim";
constexpr const char* kNvsKey7dUsed = "7d_used";
constexpr const char* kNvsKey7dLim  = "7d_lim";
constexpr const char* kNvsKeyTs     = "ts";

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

// Staleness threshold (minutes) before footer turns red.
constexpr uint32_t kStaleThresholdMin = 5;

// Footer / loop cadence.
constexpr uint32_t kFooterRefreshMs = 5000;   // 5 s tick for "updated Xm ago"

// Sentinel for "no quota ever received" (e.g. on first boot).
constexpr uint64_t kTsUnset = 0;

}  // namespace cc_hud
