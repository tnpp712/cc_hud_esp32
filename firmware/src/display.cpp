// display.cpp
// ST7789 240x240 rendering implementation.
//
// Layout (vertical):
//   0..27   header   "CC HUD" + connection dot
//   36..107 5H row   label, percentage, progress bar, used/limit
//   120..191 7D row  same layout
//   210..235 footer  "updated Xm ago" / "stale Xm"
//
// All region coordinates and colors are taken from config.h. The renderer
// tracks the last drawn percentage (per row) and the last-drawn header,
// footer and connection state so we can avoid full-screen repaints inside
// loop().

#include "display.h"

#include <TFT_eSPI.h>

#include <cstdio>
#include <cstring>

#include "config.h"

namespace cc_hud {

namespace {

// Single TFT_eSPI instance for the whole firmware.
TFT_eSPI g_tft;

// Bookkeeping for partial-redraw decisions. -1 sentinels force a paint on the
// first render after boot.
struct LastDrawnState {
    int16_t  pct_5h          = -1;
    uint16_t used_5h         = 0xFFFF;
    uint16_t limit_5h        = 0xFFFF;
    int16_t  pct_7d          = -1;
    uint16_t used_7d         = 0xFFFF;
    uint16_t limit_7d        = 0xFFFF;
    bool     ble_connected   = false;
    bool     header_drawn    = false;
    int32_t  footer_stale_min = INT32_MIN;  // sentinel for "never drawn"
    bool     footer_stale_red = false;
};

LastDrawnState g_last;

// Compute integer percentage (0..100), saturating at 100. Returns 0 if the
// limit is zero (unconfigured) so the bar stays empty rather than NaN'ing.
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

// ------------------------------------------------------------------ header --
void drawHeader(bool connected) {
    g_tft.fillRect(0, kHeaderY, kScreenWidth, kHeaderH, kColorBg);

    g_tft.setTextDatum(TL_DATUM);
    g_tft.setTextColor(kColorFg, kColorBg);
    g_tft.setTextFont(4);
    g_tft.drawString("CC HUD", kRowMargin, kHeaderY + 2);

    const uint16_t dot_color = connected ? kColorDotLink : kColorDotIdle;
    const int16_t  dot_x     = kScreenWidth - kHeaderDotPad - kHeaderDotR;
    const int16_t  dot_y     = kHeaderY + (kHeaderH / 2);
    g_tft.fillCircle(dot_x, dot_y, kHeaderDotR, dot_color);
    g_tft.drawCircle(dot_x, dot_y, kHeaderDotR, kColorFg);
}

// Update only the dot subregion (cheap path used from BLE callbacks).
void drawHeaderDot(bool connected) {
    const uint16_t dot_color = connected ? kColorDotLink : kColorDotIdle;
    const int16_t  dot_x     = kScreenWidth - kHeaderDotPad - kHeaderDotR;
    const int16_t  dot_y     = kHeaderY + (kHeaderH / 2);
    // Erase a slightly larger box to avoid ring artefacts from the previous
    // dot, then redraw.
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
// Renders one quota row ("5H" or "7D") at vertical offset `row_y` with height
// `row_h`. The layout inside a row, top to bottom:
//   * Label  (top-left, small font)
//   * Percentage (right-aligned, large font)
//   * Progress bar (horizontal, full width minus margins)
//   * "used / limit" small text under the bar.
void drawRow(const char* label,
             int16_t row_y,
             uint16_t used,
             uint16_t limit,
             uint8_t pct) {
    // Clear the row band first.
    g_tft.fillRect(0, row_y, kScreenWidth, k5hRowH, kColorBg);

    const uint16_t color = colorForPct(pct);

    // Label, small font, top-left.
    g_tft.setTextDatum(TL_DATUM);
    g_tft.setTextFont(4);
    g_tft.setTextColor(kColorMuted, kColorBg);
    g_tft.drawString(label, kRowMargin, row_y);

    // Percentage, large font, right-aligned. Buffer is generous.
    char pct_buf[8];
    std::snprintf(pct_buf, sizeof(pct_buf), "%u%%", static_cast<unsigned>(pct));
    g_tft.setTextDatum(TR_DATUM);
    g_tft.setTextFont(7);
    g_tft.setTextColor(color, kColorBg);
    g_tft.drawString(pct_buf, kScreenWidth - kRowMargin, row_y - 4);

    // Progress bar. Track first, then fill.
    const int16_t bar_x = kBarMarginX;
    const int16_t bar_y = row_y + 44;
    const int16_t bar_w = kScreenWidth - (kBarMarginX * 2);
    g_tft.fillRect(bar_x, bar_y, bar_w, kBarHeight, kColorBarTrack);
    const int16_t fill_w = (static_cast<int32_t>(bar_w) * pct) / 100;
    if (fill_w > 0) {
        g_tft.fillRect(bar_x, bar_y, fill_w, kBarHeight, color);
    }
    g_tft.drawRect(bar_x, bar_y, bar_w, kBarHeight, kColorMuted);

    // "used / limit" small text under the bar.
    char usage_buf[24];
    std::snprintf(usage_buf,
                  sizeof(usage_buf),
                  "%u / %u",
                  static_cast<unsigned>(used),
                  static_cast<unsigned>(limit));
    g_tft.setTextDatum(TL_DATUM);
    g_tft.setTextFont(2);
    g_tft.setTextColor(kColorMuted, kColorBg);
    g_tft.drawString(usage_buf, bar_x, bar_y + kBarHeight + 2);
}

// --------------------------------------------------------------------- footer
//
// Renders "updated Xm ago" or "stale Xm" depending on the freshness of the
// last BLE write. If no quota has ever been received (`last_update_ms == 0`),
// renders "waiting for data...".
void drawFooter(const QuotaSnapshot& quota, uint64_t now_ms) {
    g_tft.fillRect(0, kFooterY, kScreenWidth, kFooterH, kColorBg);

    g_tft.setTextDatum(TL_DATUM);
    g_tft.setTextFont(2);

    char buf[40];

    if (quota.last_update_ms == kTsUnset) {
        std::strncpy(buf, "waiting for data...", sizeof(buf));
        buf[sizeof(buf) - 1] = '\0';
        g_tft.setTextColor(kColorMuted, kColorBg);
        g_tft.drawString(buf, kRowMargin, kFooterY + 6);
        g_last.footer_stale_min = -1;  // distinct sentinel
        g_last.footer_stale_red = false;
        return;
    }

    // `now_ms` should always be >= last_update_ms since both come from millis
    // on the same MCU, but guard just in case of rollover quirks.
    const uint64_t delta_ms = (now_ms >= quota.last_update_ms)
                                  ? (now_ms - quota.last_update_ms)
                                  : 0;
    const uint32_t delta_min = static_cast<uint32_t>(delta_ms / 60000ULL);

    const bool stale = delta_min >= kStaleThresholdMin;
    if (stale) {
        std::snprintf(buf, sizeof(buf), "stale %lum", static_cast<unsigned long>(delta_min));
        g_tft.setTextColor(kColorRed, kColorBg);
    } else {
        std::snprintf(buf, sizeof(buf), "updated %lum ago", static_cast<unsigned long>(delta_min));
        g_tft.setTextColor(kColorMuted, kColorBg);
    }
    g_tft.drawString(buf, kRowMargin, kFooterY + 6);

    g_last.footer_stale_min = static_cast<int32_t>(delta_min);
    g_last.footer_stale_red = stale;
}

}  // namespace

// ---------------------------------------------------------------- public API
void displayInit() {
    g_tft.init();
    g_tft.setRotation(0);  // portrait, native 240x240
    g_tft.fillScreen(kColorBg);

    // Backlight pin — TFT_eSPI handles this when TFT_BL is configured, but
    // we force it on explicitly so the backlight comes up the same way after
    // a soft-reset.
    pinMode(kPinLcdBl, OUTPUT);
    digitalWrite(kPinLcdBl, HIGH);

    g_last = LastDrawnState{};
}

void displayRender(const DisplayView& view, bool full_redraw) {
    // ---- header ----
    if (full_redraw || !g_last.header_drawn ||
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
    // Always recompute since freshness changes over time.
    drawFooter(view.quota, view.now_ms);
}

void displayTickFooter(const QuotaSnapshot& quota, uint64_t now_ms) {
    // Compute the minute counter we *would* show, and skip the actual paint
    // if it would be identical to what is already on screen.
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
