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
// the first render after boot. *_label fields cache the formatted strings so
// we can skip a row repaint when the user-visible text is identical even if
// internal numbers nudged sub-minute.
struct LastDrawnState {
    // Header.
    char     title[33]            = {0};
    bool     header_drawn         = false;
    bool     ble_connected        = false;

    // Subscription-mode body.
    int16_t  pct_5h               = -1;
    uint16_t used_5h              = 0xFFFF;
    uint16_t limit_5h             = 0xFFFF;
    char     reset_label_5h[16]   = {0};
    int16_t  pct_7d               = -1;
    uint16_t used_7d              = 0xFFFF;
    uint16_t limit_7d             = 0xFFFF;
    char     reset_label_7d[16]   = {0};

    // API-mode body.
    uint32_t cost_micro_usd       = 0xFFFFFFFFu;
    uint32_t duration_s           = 0xFFFFFFFFu;

    // Which mode the body region currently holds (so we know to wipe it
    // when the mode flips).
    int8_t   body_mode_drawn      = -1;  // -1 = nothing drawn yet

    // Footer.
    int32_t  footer_stale_min     = INT32_MIN;
    bool     footer_stale_red     = false;
};
LastDrawnState g_last;

// Format the remaining-until-reset seconds into a short label, e.g.
// "" (zero/unknown), "45s", "12m", "4h12m", "5d". Buffer must be >= 8 bytes.
void formatRemaining(uint32_t remaining_s, char* out, size_t out_sz) {
    if (out_sz == 0) return;
    if (remaining_s == 0) {
        out[0] = '\0';
        return;
    }
    if (remaining_s < 60) {
        std::snprintf(out, out_sz, "%us", static_cast<unsigned>(remaining_s));
        return;
    }
    if (remaining_s < 3600) {
        std::snprintf(out, out_sz, "%um", static_cast<unsigned>(remaining_s / 60));
        return;
    }
    if (remaining_s < 86400) {
        const unsigned h = remaining_s / 3600;
        const unsigned m = (remaining_s % 3600) / 60;
        std::snprintf(out, out_sz, "%uh%um", h, m);
        return;
    }
    const unsigned d = remaining_s / 86400;
    const unsigned h = (remaining_s % 86400) / 3600;
    if (h == 0) {
        std::snprintf(out, out_sz, "%ud", d);
    } else {
        std::snprintf(out, out_sz, "%ud%uh", d, h);
    }
}

// Apply the elapsed-since-capture decrement so the countdown stays live
// between BLE pushes. Saturates at 0.
uint32_t liveRemainingS(uint32_t captured_s, uint64_t capture_ms, uint64_t now_ms) {
    if (captured_s == 0) {
        return 0;
    }
    const uint64_t elapsed_ms = (now_ms >= capture_ms) ? (now_ms - capture_ms) : 0;
    const uint32_t elapsed_s  = static_cast<uint32_t>(elapsed_ms / 1000ULL);
    return (elapsed_s >= captured_s) ? 0u : (captured_s - elapsed_s);
}

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
//
// The header bar shows the active plan / subscription level ("Plan Max (20x)",
// "API", "Free", whatever the host writes in the v3 payload). On the right
// there's a small dot indicating BLE connection state.
//
// Long titles are truncated graphically — we don't shrink the font.
void drawHeader(const char* title, bool connected) {
    g_tft.fillRect(0, kHeaderY, kScreenWidth, kHeaderH, kColorBg);

    // Reserve a strip on the right for the connection dot so the title never
    // overlaps it.
    const int16_t max_title_w = kScreenWidth - kHeaderDotPad - (kHeaderDotR * 2)
                                - kRowMargin - 4;
    g_tft.setFont(&FreeSansBold12pt7b);
    g_tft.setTextColor(kColorFg);
    g_tft.setCursor(kRowMargin, kHeaderY + 22);
    // Manually clip to max_title_w using getTextBounds on a growing prefix.
    int16_t  x1, y1;
    uint16_t w, h;
    g_tft.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
    if (w <= max_title_w) {
        g_tft.print(title);
    } else {
        // Slow path: print char-by-char until we'd exceed max_title_w.
        char buf[2] = {0, 0};
        int16_t cursor_x = kRowMargin;
        for (const char* p = title; *p; ++p) {
            buf[0] = *p;
            g_tft.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
            if (cursor_x + static_cast<int16_t>(w) > kRowMargin + max_title_w) {
                break;
            }
            g_tft.print(buf);
            cursor_x += static_cast<int16_t>(w);
        }
    }

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
             const char* reset_label,
             int16_t row_y,
             uint16_t used, uint16_t limit,
             uint8_t pct) {
    g_tft.fillRect(0, row_y, kScreenWidth, k5hRowH, kColorBg);

    const uint16_t color = colorForPct(pct);

    // Label (small bold, top-left).
    printAt(label,
            kRowMargin, row_y + 22,
            kColorMuted, &FreeSansBold12pt7b);

    // Reset countdown next to the label — tiny font, muted color. Only drawn
    // when the host supplied a non-empty string.
    if (reset_label && reset_label[0] != '\0') {
        // Get width of label so we know where to put the countdown.
        g_tft.setFont(&FreeSansBold12pt7b);
        int16_t  x1, y1;
        uint16_t lw, lh;
        g_tft.getTextBounds(label, 0, 0, &x1, &y1, &lw, &lh);
        printAt(reset_label,
                kRowMargin + static_cast<int16_t>(lw) + 8,
                row_y + 22,
                kColorMuted, &FreeSans9pt7b);
    }

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

// API-mode body: big "$X.YZ" cost in the centre and a "t: 1m20s" duration
// below. Replaces the two quota rows when mode == kSnapshotModeApi.
void formatDuration(uint32_t total_s, char* out, size_t out_sz) {
    if (out_sz == 0) return;
    if (total_s >= 3600) {
        const unsigned h = total_s / 3600;
        const unsigned m = (total_s % 3600) / 60;
        std::snprintf(out, out_sz, "%uh%um", h, m);
    } else if (total_s >= 60) {
        const unsigned m = total_s / 60;
        const unsigned s = total_s % 60;
        std::snprintf(out, out_sz, "%um%us", m, s);
    } else {
        std::snprintf(out, out_sz, "%us", static_cast<unsigned>(total_s));
    }
}

void drawApiBody(uint32_t cost_micro_usd, uint32_t duration_s) {
    // Wipe the entire 5h+7d body region.
    g_tft.fillRect(0, k5hRowY,
                   kScreenWidth, (k7dRowY + k7dRowH) - k5hRowY,
                   kColorBg);

    // Cost: format as $X.XX (two decimals). 1_000_000 micro-USD = $1.00.
    char cost_buf[16];
    const unsigned dollars  = cost_micro_usd / 1000000u;
    const unsigned cents100 = (cost_micro_usd % 1000000u) / 10000u;  // 0..99
    std::snprintf(cost_buf, sizeof(cost_buf), "$%u.%02u", dollars, cents100);
    // Center horizontally, baseline a bit below the row midpoint.
    g_tft.setFont(&FreeSansBold24pt7b);
    int16_t  x1, y1;
    uint16_t w, h;
    g_tft.getTextBounds(cost_buf, 0, 0, &x1, &y1, &w, &h);
    const int16_t cx = (kScreenWidth - static_cast<int16_t>(w)) / 2;
    const int16_t cy = k5hRowY + 60;
    printAt(cost_buf, cx, cy, kColorGreen, &FreeSansBold24pt7b);

    // Duration label.
    char dur_buf[24];
    if (duration_s == 0) {
        std::strncpy(dur_buf, "", sizeof(dur_buf));
    } else {
        char tmp[16];
        formatDuration(duration_s, tmp, sizeof(tmp));
        std::snprintf(dur_buf, sizeof(dur_buf), "t: %s", tmp);
    }
    if (dur_buf[0] != '\0') {
        g_tft.setFont(&FreeSans9pt7b);
        g_tft.getTextBounds(dur_buf, 0, 0, &x1, &y1, &w, &h);
        const int16_t dx = (kScreenWidth - static_cast<int16_t>(w)) / 2;
        printAt(dur_buf, dx, k7dRowY + 50, kColorMuted, &FreeSans9pt7b);
    }
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
    const QuotaSnapshot& q = view.quota;
    const uint8_t mode = q.mode;

    // ---- header ----
    const bool title_changed = std::strncmp(g_last.title, q.title,
                                            sizeof(g_last.title)) != 0;
    if (full_redraw ||
        !g_last.header_drawn ||
        title_changed ||
        g_last.ble_connected != view.ble_connected) {
        drawHeader(q.title, view.ble_connected);
        g_last.header_drawn  = true;
        g_last.ble_connected = view.ble_connected;
        std::strncpy(g_last.title, q.title, sizeof(g_last.title) - 1);
        g_last.title[sizeof(g_last.title) - 1] = '\0';
    }

    // If we're switching modes, wipe both rows and reset their caches.
    const bool mode_flip = (g_last.body_mode_drawn != -1) &&
                           (g_last.body_mode_drawn != static_cast<int8_t>(mode));
    const bool body_full = full_redraw || mode_flip || g_last.body_mode_drawn == -1;
    if (mode_flip) {
        g_tft.fillRect(0, k5hRowY,
                       kScreenWidth, (k7dRowY + k7dRowH) - k5hRowY, kColorBg);
        g_last.pct_5h = -1; g_last.pct_7d = -1;
        g_last.used_5h = 0xFFFF; g_last.limit_5h = 0xFFFF;
        g_last.used_7d = 0xFFFF; g_last.limit_7d = 0xFFFF;
        g_last.reset_label_5h[0] = '\0';
        g_last.reset_label_7d[0] = '\0';
        g_last.cost_micro_usd = 0xFFFFFFFFu;
        g_last.duration_s     = 0xFFFFFFFFu;
    }

    if (mode == kSnapshotModeApi) {
        // ---- API body: cost + duration ----
        if (body_full ||
            g_last.cost_micro_usd != q.cost_micro_usd ||
            g_last.duration_s     != q.duration_s) {
            drawApiBody(q.cost_micro_usd, q.duration_s);
            g_last.cost_micro_usd = q.cost_micro_usd;
            g_last.duration_s     = q.duration_s;
        }
    } else {
        // ---- Subscription body: 5H + 7D rows with reset countdowns ----
        char reset_5h[16];
        char reset_7d[16];
        formatRemaining(
            liveRemainingS(q.reset_in_s_5h, q.last_update_ms, view.now_ms),
            reset_5h, sizeof(reset_5h));
        formatRemaining(
            liveRemainingS(q.reset_in_s_7d, q.last_update_ms, view.now_ms),
            reset_7d, sizeof(reset_7d));

        const uint8_t pct_5h = computePercent(q.used_5h, q.limit_5h);
        if (body_full ||
            g_last.pct_5h   != pct_5h ||
            g_last.used_5h  != q.used_5h ||
            g_last.limit_5h != q.limit_5h ||
            std::strncmp(g_last.reset_label_5h, reset_5h,
                         sizeof(g_last.reset_label_5h)) != 0) {
            drawRow("5H", reset_5h, k5hRowY, q.used_5h, q.limit_5h, pct_5h);
            g_last.pct_5h   = pct_5h;
            g_last.used_5h  = q.used_5h;
            g_last.limit_5h = q.limit_5h;
            std::strncpy(g_last.reset_label_5h, reset_5h,
                         sizeof(g_last.reset_label_5h) - 1);
            g_last.reset_label_5h[sizeof(g_last.reset_label_5h) - 1] = '\0';
        }

        const uint8_t pct_7d = computePercent(q.used_7d, q.limit_7d);
        if (body_full ||
            g_last.pct_7d   != pct_7d ||
            g_last.used_7d  != q.used_7d ||
            g_last.limit_7d != q.limit_7d ||
            std::strncmp(g_last.reset_label_7d, reset_7d,
                         sizeof(g_last.reset_label_7d)) != 0) {
            drawRow("7D", reset_7d, k7dRowY, q.used_7d, q.limit_7d, pct_7d);
            g_last.pct_7d   = pct_7d;
            g_last.used_7d  = q.used_7d;
            g_last.limit_7d = q.limit_7d;
            std::strncpy(g_last.reset_label_7d, reset_7d,
                         sizeof(g_last.reset_label_7d) - 1);
            g_last.reset_label_7d[sizeof(g_last.reset_label_7d) - 1] = '\0';
        }
    }

    g_last.body_mode_drawn = static_cast<int8_t>(mode);

    // ---- footer ----
    drawFooter(q, view.now_ms);
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
