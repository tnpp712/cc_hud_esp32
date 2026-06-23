// display.cpp
// ST7789 panel bring-up + the OTA progress screen. Everything else —
// HUD, idle clock, pet, state slot — renders through LVGL (lvgl_ui.cpp);
// this file only owns what must work when LVGL is NOT ticking:
//
//   * displayInit()      — SPI + panel init (LVGL's flush callback pushes
//                          pixels through the same Adafruit_ST7789 object,
//                          exposed via displayGetTft()).
//   * the OTA screen     — drawn directly from the BLE OTA task while the
//                          main loop has stopped pumping LVGL. The device
//                          reboots when OTA ends, so there's no hand-back.
//
// Why Adafruit not TFT_eSPI: on this ESP32-S3R8 clone TFT_eSPI's tft.init()
// crashed before any pixel was drawn (verified via boot-loop traces). Adafruit
// gives us explicit control over the SPIClass and SPI mode, which made the
// panel come up cleanly during hardware bring-up.
//
// The legacy full-screen renderer (~900 lines of hand-drawn HUD/idle/pet/
// state painting with manual diff caches) was removed after the LVGL
// migration proved out on hardware — see git history if you need it back.

#include "display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include <cstdio>

#include "config.h"

namespace cc_hud {

namespace {

// Dedicated HSPI bus (SPI2). Leaves FSPI free for any future expansion.
SPIClass        g_spi(HSPI);
Adafruit_ST7789 g_tft(&g_spi, kPinLcdCs, kPinLcdDc, kPinLcdRst);

// Adafruit_GFX FreeFonts use the cursor (x, y) as the *baseline*, not the
// top-left; callers pass baseline coordinates.
void printAt(const char* s,
             int16_t x, int16_t y_baseline,
             uint16_t color,
             const GFXfont* font) {
    g_tft.setFont(font);
    g_tft.setTextColor(color);
    g_tft.setCursor(x, y_baseline);
    g_tft.print(s);
}

}  // namespace

// Backlight PWM (night auto-dim). LEDC channel on kPinLcdBl. 12-bit so the
// low end stays smooth.
namespace {
constexpr uint8_t  kBlPwmChannel = 6;     // LEDC channel (NeoPixel uses RMT, not LEDC)
constexpr uint8_t  kBlPwmBits    = 12;
constexpr uint32_t kBlPwmFreq    = 5000;
constexpr uint32_t kBlPwmMax     = (1u << kBlPwmBits) - 1u;
uint8_t g_bl_pct = 100;
}  // namespace

// =========================================================== public API ===
void displaySetBacklight(uint8_t pct) {
    if (pct > 100) pct = 100;
    g_bl_pct = pct;
    const uint32_t duty = (kBlPwmMax * pct) / 100u;
    ledcWrite(kBlPwmChannel, duty);
}

void displayInit() {
    // Backlight via LEDC PWM so we can dim at night (Arduino-ESP32 2.x API).
    // Configure the channel but keep it DARK (duty 0 after attach) until the
    // panel is initialised and cleared — otherwise the backlight lights up the
    // uninitialised panel RAM / previous frame during SPI+panel init, which
    // reads as a boot "flash". We illuminate only after fillScreen() below.
    ledcSetup(kBlPwmChannel, kBlPwmFreq, kBlPwmBits);
    ledcAttachPin(kPinLcdBl, kBlPwmChannel);

    // SCK, MISO (unused), MOSI, CS — explicit to leave nothing to library default.
    g_spi.begin(kPinLcdSclk, /*MISO=*/-1, kPinLcdMosi, kPinLcdCs);

    g_tft.init(kScreenWidth, kScreenHeight, SPI_MODE0);
    g_tft.setSPISpeed(40 * 1000 * 1000);  // 40 MHz — verified on this wiring
    g_tft.setRotation(0);
    g_tft.fillScreen(kColorBg);

    // Panel is now cleared to the background — safe to turn the backlight on
    // without showing boot garbage.
    displaySetBacklight(100);
}

// ============================================================ OTA mode ===
//
// While an OTA is in progress (the BLE OTA task is streaming firmware into
// the inactive flash slot) we take over the screen with a big progress
// readout. The main loop checks displayIsOtaActive() and stops ticking
// LVGL so it can't overpaint us.
//
// Layout (centered):
//   y=30   "OTA UPDATE" (bold 12pt)
//   y=110  "<PCT>%"     (bold 24pt, green)
//   y=160  progress bar (kBarHeight*2 tall)
//   y=210  "<received> / <total> B" (9pt muted)

namespace {
volatile bool g_ota_active   = false;
int16_t       g_ota_last_pct = -1;

void drawOtaFrame(uint32_t received, uint32_t total, int16_t pct) {
    char buf[40];

    // Header bar.
    g_tft.fillRect(0, 0, kScreenWidth, 50, kColorBg);
    printAt("OTA UPDATE",
            kRowMargin, 30, kColorFg, &FreeSansBold12pt7b);

    // Big percentage, centered.
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(pct));
    g_tft.setFont(&FreeSansBold24pt7b);
    int16_t  x1, y1;
    uint16_t w, h;
    g_tft.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    g_tft.fillRect(0, 80, kScreenWidth, 50, kColorBg);
    g_tft.setCursor((kScreenWidth - static_cast<int16_t>(w)) / 2, 120);
    g_tft.setTextColor(kColorGreen);
    g_tft.print(buf);

    // Progress bar.
    const int16_t bar_x = 20;
    const int16_t bar_y = 160;
    const int16_t bar_w = kScreenWidth - 40;
    const int16_t bar_h = kBarHeight * 2;
    g_tft.fillRect(bar_x, bar_y, bar_w, bar_h, kColorBarTrack);
    const int16_t fill_w =
        static_cast<int16_t>((static_cast<int32_t>(bar_w) * pct) / 100);
    if (fill_w > 0) {
        g_tft.fillRect(bar_x, bar_y, fill_w, bar_h, kColorGreen);
    }
    g_tft.drawRect(bar_x, bar_y, bar_w, bar_h, kColorMuted);

    // Bytes counter.
    std::snprintf(buf, sizeof(buf), "%u / %u B",
                  static_cast<unsigned>(received),
                  static_cast<unsigned>(total));
    g_tft.fillRect(0, 200, kScreenWidth, 30, kColorBg);
    g_tft.setFont(&FreeSans9pt7b);
    g_tft.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    g_tft.setCursor((kScreenWidth - static_cast<int16_t>(w)) / 2, 220);
    g_tft.setTextColor(kColorMuted);
    g_tft.print(buf);
}
}  // anon namespace

void displayBeginOta() {
    g_ota_active   = true;
    g_ota_last_pct = -1;
    g_tft.fillScreen(kColorBg);
    drawOtaFrame(0, 1, 0);
}

void displayOtaProgress(uint32_t received, uint32_t total) {
    if (!g_ota_active) {
        // First call without an explicit begin — set up the screen first.
        displayBeginOta();
    }
    if (total == 0) return;
    int16_t pct = static_cast<int16_t>(
        (static_cast<uint64_t>(received) * 100ULL) / total);
    if (pct > 100) pct = 100;
    if (pct == g_ota_last_pct) {
        return;  // cheap path: nothing to redraw at this granularity
    }
    g_ota_last_pct = pct;
    drawOtaFrame(received, total, pct);
}

bool displayIsOtaActive() {
    return g_ota_active;
}

Adafruit_ST7789& displayGetTft() {
    return g_tft;
}

}  // namespace cc_hud
