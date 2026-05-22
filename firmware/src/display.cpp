// display.cpp
// ST7789 240x240 rendering, Adafruit_GFX + Adafruit_ST7789 backed.
//
// Why Adafruit not TFT_eSPI: on this ESP32-S3R8 clone TFT_eSPI's tft.init()
// crashed before any pixel was drawn (verified via boot-loop traces). Adafruit
// gives us explicit control over the SPIClass and SPI mode, which made the
// panel come up cleanly during hardware bring-up.
//
// Layout (vertical, 240x240, all coordinates from config.h):
//   0..27    header   "CC HUD" + connection dot
//   36..107  5H row   label + big percentage + bar + "used / limit"
//   120..191 7D row   same layout
//   210..235 footer   "updated Xm ago" / "stale Xm" / "waiting for data..."
//
// Partial redraw: a LastDrawnState snapshot avoids fillScreen on every tick.
// Only regions whose inputs changed get repainted.

#include "display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include <cstdio>
#include <cstring>
#include <climits>

#include "config.h"

namespace cc_hud {

namespace {

// Dedicated HSPI bus (SPI2). Leaves FSPI free for any future expansion.
SPIClass        g_spi(HSPI);
Adafruit_ST7789 g_tft(&g_spi, kPinLcdCs, kPinLcdDc, kPinLcdRst);

// Bookkeeping for partial-redraw. INT_MIN / 0xFFFF sentinels force a paint on
// the first render after boot.
struct LastDrawnState {
    int16_t  pct_5h           = -1;
    uint16_t used_5h          = 0xFFFF;
    uint16_t limit_5h         = 0xFFFF;
    int16_t  pct_7d           = -1;
    uint16_t used_7d          = 0xFFFF;
    uint16_t limit_7d         = 0xFFFF;
    bool     ble_connected    = false;
    bool     header_drawn     = false;
    int32_t  footer_stale_min = INT32_MIN;
    bool     footer_stale_red = false;
};
LastDrawnState g_last;

// ---------------------------------------------------------------- math/utils
uint8_t computePercent(uint16_t used, uint16_t limit) {
    if (limit == 0) {
        return 0;
    }
    const uint32_t pct = (static_cast<uint32_t>(used) * 100u) / limit;
    return pct > 100u ? 100u : static_cast<uint8_t>(pct);
}

uint16_t colorForPct(uint8_t pct) {
    if (pct <= kThreshGreenMax) {
        return kColorGreen;
    }
    if (pct <= kThreshYellowMax) {
        return kColorYellow;
    }
    return kColorRed;
}

// ------------------------------------------------------------------ text --
//
// Adafruit_GFX FreeFonts use the cursor (x, y) as the *baseline*, not the
// top-left. We supply baseline coordinates throughout, and helper wrappers
// compute right-aligned positions via getTextBounds().

void printAt(const char* s,
             int16_t x, int16_t y_baseline,
             uint16_t color,
             const GFXfont* font) {
    g_tft.setFont(font);
    g_tft.setTextColor(color);
    g_tft.setCursor(x, y_baseline);
    g_tft.print(s);
}

void printRight(const char* s,
                int16_t x_right, int16_t y_baseline,
                uint16_t color,
                const GFXfont* font) {
    g_tft.setFont(font);
    g_tft.setTextColor(color);
    int16_t  x1, y1;
    uint16_t w, h;
    g_tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    g_tft.setCursor(x_right - static_cast<int16_t>(w), y_baseline);
    g_tft.print(s);
}

// ------------------------------------------------------------------ header --
void drawHeader(bool connected) {
    g_tft.fillRect(0, kHeaderY, kScreenWidth, kHeaderH, kColorBg);
    printAt("CC HUD",
            kRowMargin, kHeaderY + 22,
            kColorFg, &FreeSansBold12pt7b);

    const uint16_t dot_color = connected ? kColorDotLink : kColorDotIdle;
    const int16_t  dot_x     = kScreenWidth - kHeaderDotPad - kHeaderDotR;
    const int16_t  dot_y     = kHeaderY + (kHeaderH / 2);
    g_tft.fillCircle(dot_x, dot_y, kHeaderDotR, dot_color);
    g_tft.drawCircle(dot_x, dot_y, kHeaderDotR, kColorFg);
}

void drawHeaderDot(bool connected) {
    const uint16_t dot_color = connected ? kColorDotLink : kColorDotIdle;
    const int16_t  dot_x     = kScreenWidth - kHeaderDotPad - kHeaderDotR;
    const int16_t  dot_y     = kHeaderY + (kHeaderH / 2);
    // Wipe a slightly larger box to avoid ring artefacts before redrawing.
    g_tft.fillRect(dot_x - kHeaderDotR - 2,
                   dot_y - kHeaderDotR - 2,
                   (kHeaderDotR + 2) * 2,
                   (kHeaderDotR + 2) * 2,
                   kColorBg);
    g_tft.fillCircle(dot_x, dot_y, kHeaderDotR, dot_color);
    g_tft.drawCircle(dot_x, dot_y, kHeaderDotR, kColorFg);
}

// --------------------------------------------------------------------- row --
//
// Row vertical structure (row height 72):
//   y+0..y+30   label baseline @ y+22  | percentage baseline @ y+34
//   y+44..y+57  progress bar (kBarHeight=14)
//   y+59..y+71  used/limit small text baseline @ y+72
void drawRow(const char* label,
             int16_t row_y,
             uint16_t used, uint16_t limit,
             uint8_t pct) {
    g_tft.fillRect(0, row_y, kScreenWidth, k5hRowH, kColorBg);

    const uint16_t color = colorForPct(pct);

    // Label (small bold, top-left).
    printAt(label,
            kRowMargin, row_y + 22,
            kColorMuted, &FreeSansBold12pt7b);

    // Big percentage (right-aligned, big bold). Baseline at row_y + 34
    // sits inside the upper half of the row.
    char pct_buf[8];
    std::snprintf(pct_buf, sizeof(pct_buf), "%u%%", static_cast<unsigned>(pct));
    printRight(pct_buf,
               kScreenWidth - kRowMargin, row_y + 36,
               color, &FreeSansBold24pt7b);

    // Progress bar.
    const int16_t bar_x = kBarMarginX;
    const int16_t bar_y = row_y + 44;
    const int16_t bar_w = kScreenWidth - (kBarMarginX * 2);
    g_tft.fillRect(bar_x, bar_y, bar_w, kBarHeight, kColorBarTrack);
    const int16_t fill_w =
        static_cast<int16_t>((static_cast<int32_t>(bar_w) * pct) / 100);
    if (fill_w > 0) {
        g_tft.fillRect(bar_x, bar_y, fill_w, kBarHeight, color);
    }
    g_tft.drawRect(bar_x, bar_y, bar_w, kBarHeight, kColorMuted);

    // "used / limit" under the bar.
    char usage_buf[24];
    std::snprintf(usage_buf, sizeof(usage_buf),
                  "%u / %u",
                  static_cast<unsigned>(used),
                  static_cast<unsigned>(limit));
    printAt(usage_buf,
            bar_x, bar_y + kBarHeight + 14,
            kColorMuted, &FreeSans9pt7b);
}

// ---------------------------------------------------------------- footer --
void drawFooter(const QuotaSnapshot& quota, uint64_t now_ms) {
    g_tft.fillRect(0, kFooterY, kScreenWidth, kFooterH, kColorBg);

    char buf[40];
    uint16_t color;

    if (quota.last_update_ms == kTsUnset) {
        std::strncpy(buf, "waiting for data...", sizeof(buf));
        buf[sizeof(buf) - 1] = '\0';
        color = kColorMuted;
        g_last.footer_stale_min = -1;
        g_last.footer_stale_red = false;
    } else {
        const uint64_t delta_ms = (now_ms >= quota.last_update_ms)
                                      ? (now_ms - quota.last_update_ms)
                                      : 0;
        const uint32_t delta_min =
            static_cast<uint32_t>(delta_ms / 60000ULL);
        const bool stale = delta_min >= kStaleThresholdMin;
        if (stale) {
            std::snprintf(buf, sizeof(buf), "stale %lum",
                          static_cast<unsigned long>(delta_min));
            color = kColorRed;
        } else {
            std::snprintf(buf, sizeof(buf), "updated %lum ago",
                          static_cast<unsigned long>(delta_min));
            color = kColorMuted;
        }
        g_last.footer_stale_min = static_cast<int32_t>(delta_min);
        g_last.footer_stale_red = stale;
    }

    printAt(buf, kRowMargin, kFooterY + 18, color, &FreeSans9pt7b);
}

}  // namespace

// =========================================================== public API ===
void displayInit() {
    pinMode(kPinLcdBl, OUTPUT);
    digitalWrite(kPinLcdBl, HIGH);

    // SCK, MISO (unused), MOSI, CS — explicit to leave nothing to library default.
    g_spi.begin(kPinLcdSclk, /*MISO=*/-1, kPinLcdMosi, kPinLcdCs);

    g_tft.init(kScreenWidth, kScreenHeight, SPI_MODE0);
    g_tft.setSPISpeed(10 * 1000 * 1000);  // 10 MHz — conservative, dupont-safe
    g_tft.setRotation(0);
    g_tft.fillScreen(kColorBg);

    g_last = LastDrawnState{};
}

void displayRender(const DisplayView& view, bool full_redraw) {
    // ---- header ----
    if (full_redraw ||
        !g_last.header_drawn ||
        g_last.ble_connected != view.ble_connected) {
        drawHeader(view.ble_connected);
        g_last.header_drawn  = true;
        g_last.ble_connected = view.ble_connected;
    }

    // ---- 5H row ----
    const uint8_t pct_5h = computePercent(view.quota.used_5h, view.quota.limit_5h);
    if (full_redraw ||
        g_last.pct_5h   != pct_5h ||
        g_last.used_5h  != view.quota.used_5h ||
        g_last.limit_5h != view.quota.limit_5h) {
        drawRow("5H", k5hRowY, view.quota.used_5h, view.quota.limit_5h, pct_5h);
        g_last.pct_5h   = pct_5h;
        g_last.used_5h  = view.quota.used_5h;
        g_last.limit_5h = view.quota.limit_5h;
    }

    // ---- 7D row ----
    const uint8_t pct_7d = computePercent(view.quota.used_7d, view.quota.limit_7d);
    if (full_redraw ||
        g_last.pct_7d   != pct_7d ||
        g_last.used_7d  != view.quota.used_7d ||
        g_last.limit_7d != view.quota.limit_7d) {
        drawRow("7D", k7dRowY, view.quota.used_7d, view.quota.limit_7d, pct_7d);
        g_last.pct_7d   = pct_7d;
        g_last.used_7d  = view.quota.used_7d;
        g_last.limit_7d = view.quota.limit_7d;
    }

    // ---- footer ----
    drawFooter(view.quota, view.now_ms);
}

void displayTickFooter(const QuotaSnapshot& quota, uint64_t now_ms) {
    // Skip if the minute counter has not changed.
    if (quota.last_update_ms == kTsUnset) {
        if (g_last.footer_stale_min == -1) {
            return;
        }
    } else {
        const uint64_t delta_ms = (now_ms >= quota.last_update_ms)
                                      ? (now_ms - quota.last_update_ms)
                                      : 0;
        const uint32_t delta_min = static_cast<uint32_t>(delta_ms / 60000ULL);
        const bool     stale     = delta_min >= kStaleThresholdMin;
        if (static_cast<int32_t>(delta_min) == g_last.footer_stale_min &&
            stale == g_last.footer_stale_red) {
            return;
        }
    }
    drawFooter(quota, now_ms);
}

void displayUpdateConnection(bool connected) {
    if (g_last.header_drawn && g_last.ble_connected == connected) {
        return;
    }
    drawHeaderDot(connected);
    g_last.ble_connected = connected;
    g_last.header_drawn  = true;
}

}  // namespace cc_hud
