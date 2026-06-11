// lvgl_ui.cpp
// Full LVGL 9 UI for cc_hud. Two screens:
//
//   HUD  — header (plan title + BLE dot), 5H/7D bars with color tiers
//          (or API cost in api mode), footer (freshness + logo + the
//          Claude-state slot: thinking GIF / tool icon / waiting pulse
//          / idle dots).
//   IDLE — walking ASCII pet (mood-reactive), big clock, date,
//          separator, status string (weather).
//
// Assets reuse the exact RGB565 arrays the legacy renderer used
// (logo_brand.h / tool_icons.h / claude_star_frames.h) — wrapped in
// lv_image_dsc_t at init, zero new flash.

#if CCHUD_LVGL_UI

#include <Arduino.h>
#include <Adafruit_ST7789.h>
#include <lvgl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "display.h"
#include "lvgl_ui.h"

#include "claude_star_frames.h"
#include "logo_brand.h"
#include "tool_icons.h"

// Generated 20 px CJK subset (weather terms + major cities + full
// ASCII + °/℃/℉). See tools note in the commit: regenerate with
// lv_font_conv if the glyph set needs to grow.
LV_FONT_DECLARE(font_cn_20);

namespace cc_hud {

namespace {

// ── display driver ─────────────────────────────────────────────
constexpr int kBufRows = 40;
alignas(4) uint8_t g_buf_a[kScreenWidth * kBufRows * 2];
alignas(4) uint8_t g_buf_b[kScreenWidth * kBufRows * 2];

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    displayGetTft().drawRGBBitmap(area->x1, area->y1,
                                   reinterpret_cast<uint16_t*>(px_map),
                                   w, h);
    lv_display_flush_ready(disp);
}

uint32_t tick_cb() { return millis(); }

// ── colors (RGB888 for LVGL styles, mirroring config.h RGB565) ──
const lv_color_t C_BG     = lv_color_black();
const lv_color_t C_FG     = lv_color_white();
const lv_color_t C_MUTED  = lv_color_hex(0x7B7D75);   // ≈ kColorMuted
const lv_color_t C_GREEN  = lv_color_hex(0x00FF00);
const lv_color_t C_YELLOW = lv_color_hex(0xFFFF00);
const lv_color_t C_RED    = lv_color_hex(0xFF0000);
const lv_color_t C_TRACK  = lv_color_hex(0x222222);
const lv_color_t C_CLAUDE = lv_color_hex(0xD77757);
const lv_color_t C_CLAUDE_DIM = lv_color_hex(0x4A2A1E);
const lv_color_t C_DOT_IDLE   = lv_color_hex(0x555555);

// ── image descriptors (wrap existing RGB565 arrays) ────────────
lv_image_dsc_t g_logo_dsc;
lv_image_dsc_t g_icon_dsc[kIconCount];
lv_image_dsc_t g_star_dsc[kStarFrameCount];
const void*    g_star_src[kStarFrameCount];

void initImageDsc(lv_image_dsc_t& d, const uint16_t* px, int w, int h) {
    std::memset(&d, 0, sizeof(d));
    d.header.magic  = LV_IMAGE_HEADER_MAGIC;
    d.header.cf     = LV_COLOR_FORMAT_RGB565;
    d.header.w      = w;
    d.header.h      = h;
    d.header.stride = w * 2;
    d.data_size     = static_cast<uint32_t>(w) * h * 2;
    d.data          = reinterpret_cast<const uint8_t*>(px);
}

// ── widget handles ─────────────────────────────────────────────
lv_obj_t* g_scr_hud  = nullptr;
lv_obj_t* g_scr_idle = nullptr;

// HUD
lv_obj_t* g_title    = nullptr;
lv_obj_t* g_ble_dot  = nullptr;
lv_obj_t* g_row5_pct = nullptr;
lv_obj_t* g_row5_bar = nullptr;
lv_obj_t* g_row5_sub = nullptr;
lv_obj_t* g_row7_pct = nullptr;
lv_obj_t* g_row7_bar = nullptr;
lv_obj_t* g_row7_sub = nullptr;
lv_obj_t* g_row5_lbl = nullptr;
lv_obj_t* g_row7_lbl = nullptr;
lv_obj_t* g_api_cost = nullptr;
lv_obj_t* g_api_dur  = nullptr;
lv_obj_t* g_footer   = nullptr;
// state slot (shared 40×40 area, one visible at a time)
lv_obj_t* g_st_gif   = nullptr;   // thinking animimg
lv_obj_t* g_st_icon  = nullptr;   // tool icon
lv_obj_t* g_st_pulse = nullptr;   // waiting circle
lv_obj_t* g_st_dots[3] = {nullptr, nullptr, nullptr};

// IDLE
lv_obj_t* g_pet      = nullptr;
lv_obj_t* g_clock    = nullptr;
lv_obj_t* g_date     = nullptr;
lv_obj_t* g_status   = nullptr;

// alert overlay
lv_obj_t* g_alert    = nullptr;

// ── cached "last applied" values (diffing) ─────────────────────
struct Applied {
    char     title[kTitleBufLen]  = {0};
    bool     conn                 = false;
    int      pct5 = -1, pct7 = -1;
    char     sub5[40] = {0}, sub7[40] = {0};
    uint8_t  hud_mode = 0xFF;     // sub / api
    char     footer[40] = {0};
    bool     footer_red = false;
    char     cost[16] = {0}, dur[24] = {0};
    int8_t   app_state = kAppStateUnset;
    int8_t   icon_idx  = -1;
    bool     idle_mode = false;
    bool     idle_init = false;
    char     clock[8] = {0}, date[24] = {0}, status[kIdleStatusMaxLen + 1] = {0};
    int8_t   mood = -1;
};
Applied g_ap;

// ── pet animation state (lv_timer driven) ──────────────────────
struct PetSprite { const char* normal; const char* blink; };
constexpr PetSprite kPetSprites[kPetMoodCount] = {
    {"(=^.^=)", "(=^_^=)"},
    {"(='.'=)", "(=-.-=)"},
    {"(=u.u=)", "(=u_u=)"},
    {"(=>.<=)", "(=>_<=)"},
    {"(=T.T=)", "(=T_T=)"},
};
struct PetMove {
    uint32_t retarget_min, retarget_max;
    int16_t  smooth;
    bool     stop_centre;
};
constexpr PetMove kPetMove[kPetMoodCount] = {
    {3500, 6500, 12, false},
    {3000, 6000, 14, false},
    {4500, 8500, 20, false},
    {1200, 2500,  6, false},
    {99999, 99999, 1, true},
};
constexpr int16_t kPetMinX = 30, kPetMaxX = 150;  // label x range (obj left)
int16_t  g_pet_x = 90, g_pet_target = 90;
uint32_t g_pet_next_retarget = 0;
volatile int8_t g_pet_mood = kPetMoodHappy;

void pet_timer_cb(lv_timer_t*) {
    if (!g_ap.idle_mode || g_pet == nullptr) return;
    const uint32_t now = millis();
    int8_t m = g_pet_mood;
    if (m < 0 || m >= kPetMoodCount) m = kPetMoodHappy;
    const PetMove& mv = kPetMove[m];

    if (mv.stop_centre) {
        g_pet_target = (kPetMinX + kPetMaxX) / 2;
    } else if (now >= g_pet_next_retarget) {
        g_pet_target = static_cast<int16_t>(random(kPetMinX, kPetMaxX + 1));
        g_pet_next_retarget = now +
            static_cast<uint32_t>(random(mv.retarget_min, mv.retarget_max + 1));
    }
    const int32_t delta = g_pet_target - g_pet_x;
    int16_t nx = static_cast<int16_t>(g_pet_x + delta / mv.smooth);
    if (nx == g_pet_x && delta != 0) nx += (delta > 0) ? 1 : -1;
    g_pet_x = nx;

    // Blink + bob.
    const bool blink = (now % 4500u) >= 4250u;
    const int16_t bob = (mv.stop_centre) ? 0 : ((now / 500u) & 1);
    const PetSprite& sp = kPetSprites[m];
    lv_label_set_text_static(g_pet, blink ? sp.blink : sp.normal);
    lv_obj_set_pos(g_pet, g_pet_x, 14 + bob * 2);
}

// ── waiting pulse animation ────────────────────────────────────
void pulse_anim_cb(void* obj, int32_t v) {
    lv_obj_t* o = static_cast<lv_obj_t*>(obj);
    lv_obj_set_size(o, v, v);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, 218 - v / 2, 212 - v / 2);
}

// ── helpers ────────────────────────────────────────────────────
lv_obj_t* mkLabel(lv_obj_t* parent, const lv_font_t* font,
                  lv_color_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

lv_obj_t* mkPlainScreen() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t* mkCircle(lv_obj_t* parent, int d, lv_color_t color) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* mkBar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* b = lv_bar_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, 100);
    lv_obj_set_style_bg_color(b, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 3, LV_PART_INDICATOR);
    return b;
}

lv_color_t tierColor(int pct) {
    if (pct <= kThreshGreenMax)  return C_GREEN;
    if (pct <= kThreshYellowMax) return C_YELLOW;
    return C_RED;
}

void fmtRemaining(uint32_t s, char* out, size_t n) {
    if (s == 0)            { out[0] = '\0'; return; }
    if (s < 60)            { snprintf(out, n, "%us", (unsigned)s); return; }
    if (s < 3600)          { snprintf(out, n, "%um", (unsigned)(s / 60)); return; }
    if (s < 86400)         { snprintf(out, n, "%uh%um",
                                       (unsigned)(s / 3600),
                                       (unsigned)((s % 3600) / 60)); return; }
    snprintf(out, n, "%ud%uh", (unsigned)(s / 86400),
             (unsigned)((s % 86400) / 3600));
}

uint32_t liveRemaining(uint32_t captured_s, uint64_t cap_ms, uint64_t now_ms) {
    if (captured_s == 0) return 0;
    const uint32_t el = static_cast<uint32_t>(
        (now_ms >= cap_ms ? now_ms - cap_ms : 0) / 1000ULL);
    return el >= captured_s ? 0u : captured_s - el;
}

// ── screen builders ────────────────────────────────────────────
void buildHud() {
    g_scr_hud = mkPlainScreen();

    // Vertical budget (240 px): title 4..30, 5H block 36..110,
    // 7D block 116..190, logo/state 192..232, footer 214..230.
    g_title = mkLabel(g_scr_hud, &lv_font_montserrat_20, C_FG);
    lv_obj_set_pos(g_title, 8, 4);
    lv_label_set_text(g_title, "CC HUD");
    // Clip long plan names before they reach the BLE dot.
    lv_obj_set_width(g_title, 196);
    lv_label_set_long_mode(g_title, LV_LABEL_LONG_CLIP);

    g_ble_dot = mkCircle(g_scr_hud, 14, C_DOT_IDLE);
    lv_obj_set_pos(g_ble_dot, 212, 8);

    // 5H row
    g_row5_lbl = mkLabel(g_scr_hud, &lv_font_montserrat_18, C_FG);
    lv_obj_set_pos(g_row5_lbl, 8, 36);
    lv_label_set_text(g_row5_lbl, "5H");
    g_row5_pct = mkLabel(g_scr_hud, &lv_font_montserrat_24, C_FG);
    lv_obj_align(g_row5_pct, LV_ALIGN_TOP_RIGHT, -8, 32);
    g_row5_bar = mkBar(g_scr_hud, 8, 70, 224, 14);
    g_row5_sub = mkLabel(g_scr_hud, &lv_font_montserrat_18, C_MUTED);
    lv_obj_set_pos(g_row5_sub, 8, 88);

    // 7D row
    g_row7_lbl = mkLabel(g_scr_hud, &lv_font_montserrat_18, C_FG);
    lv_obj_set_pos(g_row7_lbl, 8, 116);
    lv_label_set_text(g_row7_lbl, "7D");
    g_row7_pct = mkLabel(g_scr_hud, &lv_font_montserrat_24, C_FG);
    lv_obj_align(g_row7_pct, LV_ALIGN_TOP_RIGHT, -8, 112);
    g_row7_bar = mkBar(g_scr_hud, 8, 150, 224, 14);
    g_row7_sub = mkLabel(g_scr_hud, &lv_font_montserrat_18, C_MUTED);
    lv_obj_set_pos(g_row7_sub, 8, 168);

    // API-mode widgets (hidden in sub mode)
    g_api_cost = mkLabel(g_scr_hud, &lv_font_montserrat_48, C_GREEN);
    lv_obj_align(g_api_cost, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_add_flag(g_api_cost, LV_OBJ_FLAG_HIDDEN);
    g_api_dur = mkLabel(g_scr_hud, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(g_api_dur, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_add_flag(g_api_dur, LV_OBJ_FLAG_HIDDEN);

    // Footer
    g_footer = mkLabel(g_scr_hud, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(g_footer, 8, 214);
    lv_label_set_text(g_footer, "waiting for data...");

    lv_obj_t* logo = lv_image_create(g_scr_hud);
    lv_image_set_src(logo, &g_logo_dsc);
    lv_obj_set_pos(logo, 156, 192);

    // State slot widgets — all at the same 40×40 spot, one shown.
    g_st_gif = lv_animimg_create(g_scr_hud);
    lv_obj_set_pos(g_st_gif, 198, 192);
    lv_obj_set_size(g_st_gif, kStarFrameW, kStarFrameH);
    lv_animimg_set_src(g_st_gif, const_cast<const void**>(g_star_src),
                       kStarFrameCount);
    lv_animimg_set_duration(g_st_gif, 100 * kStarFrameCount);
    lv_animimg_set_repeat_count(g_st_gif, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(g_st_gif);
    lv_obj_add_flag(g_st_gif, LV_OBJ_FLAG_HIDDEN);

    g_st_icon = lv_image_create(g_scr_hud);
    lv_obj_set_pos(g_st_icon, 198, 192);
    lv_obj_add_flag(g_st_icon, LV_OBJ_FLAG_HIDDEN);

    g_st_pulse = mkCircle(g_scr_hud, 16, C_CLAUDE);
    lv_obj_add_flag(g_st_pulse, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 3; ++i) {
        g_st_dots[i] = mkCircle(g_scr_hud, 6, C_CLAUDE_DIM);
        lv_obj_set_pos(g_st_dots[i], 218 - 3 + (i - 1) * 11, 212 - 3);
        lv_obj_add_flag(g_st_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void buildIdle() {
    g_scr_idle = mkPlainScreen();

    g_pet = mkLabel(g_scr_idle, &lv_font_montserrat_24, C_CLAUDE);
    lv_obj_set_pos(g_pet, 90, 14);
    lv_label_set_text_static(g_pet, kPetSprites[0].normal);

    g_clock = mkLabel(g_scr_idle, &lv_font_montserrat_48, C_FG);
    lv_obj_align(g_clock, LV_ALIGN_TOP_MID, 0, 72);
    lv_label_set_text(g_clock, "--:--");

    g_date = mkLabel(g_scr_idle, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(g_date, LV_ALIGN_TOP_MID, 0, 132);
    lv_label_set_text(g_date, "set time via BLE");

    lv_obj_t* sep = lv_obj_create(g_scr_idle);
    lv_obj_set_size(sep, 208, 1);
    lv_obj_set_pos(sep, 16, 168);
    lv_obj_set_style_bg_color(sep, C_MUTED, 0);
    lv_obj_set_style_border_width(sep, 0, 0);

    // CJK-capable font so the weather string can be Chinese
    // ("晴 +25°C 北京"). Falls back over the same glyphs for ASCII.
    g_status = mkLabel(g_scr_idle, &font_cn_20, C_GREEN);
    lv_obj_align(g_status, LV_ALIGN_TOP_MID, 0, 190);
    lv_label_set_text(g_status, "");

    lv_timer_create(pet_timer_cb, 30, nullptr);
}

void buildAlertOverlay() {
    g_alert = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_alert, kScreenWidth, kScreenHeight);
    lv_obj_set_pos(g_alert, 0, 0);
    lv_obj_set_style_bg_color(g_alert, C_RED, 0);
    lv_obj_set_style_border_width(g_alert, 0, 0);
    lv_obj_set_style_radius(g_alert, 0, 0);
    lv_obj_add_flag(g_alert, LV_OBJ_FLAG_HIDDEN);
}

void alert_opa_cb(void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj),
                         static_cast<lv_opa_t>(v), 0);
}
void alert_done_cb(lv_anim_t*) {
    lv_obj_add_flag(g_alert, LV_OBJ_FLAG_HIDDEN);
}

// ── idle clock strings ─────────────────────────────────────────
void composeIdle(const QuotaSnapshot& q, uint64_t now_ms,
                 char* clk, size_t clk_n, char* dt, size_t dt_n) {
    if (q.unix_ts == 0) {
        snprintf(clk, clk_n, "--:--");
        snprintf(dt, dt_n, "set time via BLE");
        return;
    }
    uint32_t el = 0;
    if (q.time_capture_ms > 0 && now_ms >= q.time_capture_ms) {
        el = static_cast<uint32_t>((now_ms - q.time_capture_ms) / 1000ULL);
    }
    time_t local = static_cast<time_t>(q.unix_ts) + el +
                   static_cast<time_t>(q.utc_offset_min) * 60;
    struct tm tmv;
    gmtime_r(&local, &tmv);
    snprintf(clk, clk_n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    static const char* W[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* M[] = {"Jan","Feb","Mar","Apr","May","Jun",
                              "Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(dt, dt_n, "%s %s %d %d",
             W[tmv.tm_wday % 7], M[tmv.tm_mon % 12],
             tmv.tm_mday, 1900 + tmv.tm_year);
}

int8_t iconForTool(const char* name) {
    if (!name || !*name) return -1;
    int8_t best = -1; size_t blen = 0;
    for (int i = 0; i < kIconMapLen; ++i) {
        const size_t pl = std::strlen(kIconMap[i].prefix);
        if (std::strncmp(name, kIconMap[i].prefix, pl) == 0 && pl > blen) {
            best = kIconMap[i].icon_idx; blen = pl;
        }
    }
    return best;
}

void setStateSlot(int8_t state, const char* detail) {
    lv_obj_add_flag(g_st_gif,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_st_icon,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_st_pulse, LV_OBJ_FLAG_HIDDEN);
    for (auto* d : g_st_dots) lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    lv_anim_delete(g_st_pulse, pulse_anim_cb);

    switch (state) {
        case kAppStateThinking:
            lv_obj_remove_flag(g_st_gif, LV_OBJ_FLAG_HIDDEN);
            break;
        case kAppStateTool: {
            int8_t idx = iconForTool(detail);
            if (idx < 0) idx = kIconCount - 1;     // MCP catch-all
            if (idx != g_ap.icon_idx) {
                lv_image_set_src(g_st_icon, &g_icon_dsc[idx]);
                g_ap.icon_idx = idx;
            }
            lv_obj_remove_flag(g_st_icon, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case kAppStateWaiting: {
            lv_obj_remove_flag(g_st_pulse, LV_OBJ_FLAG_HIDDEN);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, g_st_pulse);
            lv_anim_set_exec_cb(&a, pulse_anim_cb);
            lv_anim_set_values(&a, 8, 26);
            lv_anim_set_duration(&a, 600);
            lv_anim_set_playback_duration(&a, 600);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
            break;
        }
        case kAppStateIdle:
        default:
            for (auto* d : g_st_dots) lv_obj_remove_flag(d, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

}  // namespace

// ── public API ─────────────────────────────────────────────────
void lvglUiInit() {
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t* disp = lv_display_create(kScreenWidth, kScreenHeight);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, g_buf_a, g_buf_b, sizeof(g_buf_a),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    initImageDsc(g_logo_dsc, kLogo, kLogoW, kLogoH);
    for (int i = 0; i < kIconCount; ++i) {
        initImageDsc(g_icon_dsc[i], kIcons[i], kIconW, kIconH);
    }
    for (int i = 0; i < kStarFrameCount; ++i) {
        initImageDsc(g_star_dsc[i], kStarFrames[i], kStarFrameW, kStarFrameH);
        g_star_src[i] = &g_star_dsc[i];
    }

    buildHud();
    buildIdle();
    buildAlertOverlay();
    lv_screen_load(g_scr_hud);
    Serial.println("[LVGL] full UI up (HUD + idle screens)");
}

void lvglUiTick() {
    lv_timer_handler();
}

void lvglUiFlashAlert() {
    lv_obj_remove_flag(g_alert, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_alert);
    lv_anim_set_exec_cb(&a, alert_opa_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, kAlertFlashIntervalMs);
    lv_anim_set_playback_duration(&a, kAlertFlashIntervalMs);
    lv_anim_set_repeat_count(&a, kAlertFlashCycles);
    lv_anim_set_completed_cb(&a, alert_done_cb);
    lv_anim_start(&a);
}

void lvglUiApply(const LvglUiModel& m) {
    const QuotaSnapshot& q = m.quota;

    // ── screen switch ──
    if (m.idle_mode != g_ap.idle_mode || !g_ap.idle_init) {
        lv_screen_load_anim(m.idle_mode ? g_scr_idle : g_scr_hud,
                            LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
        g_ap.idle_mode = m.idle_mode;
        g_ap.idle_init = true;
    }

    g_pet_mood = static_cast<int8_t>(m.mood);

    if (m.idle_mode) {
        // ── idle screen ──
        char clk[8], dt[24];
        composeIdle(q, m.now_ms, clk, sizeof(clk), dt, sizeof(dt));
        if (std::strcmp(clk, g_ap.clock) != 0) {
            lv_label_set_text(g_clock, clk);
            std::strcpy(g_ap.clock, clk);
        }
        if (std::strcmp(dt, g_ap.date) != 0) {
            lv_label_set_text(g_date, dt);
            std::strcpy(g_ap.date, dt);
        }
        if (std::strncmp(q.idle_status, g_ap.status, sizeof(g_ap.status)) != 0) {
            lv_label_set_text(g_status, q.idle_status);
            std::strncpy(g_ap.status, q.idle_status, sizeof(g_ap.status) - 1);
        }
        return;
    }

    // ── HUD screen ──
    if (std::strncmp(q.title, g_ap.title, sizeof(g_ap.title)) != 0) {
        lv_label_set_text(g_title, q.title[0] ? q.title : "CC HUD");
        std::strncpy(g_ap.title, q.title, sizeof(g_ap.title) - 1);
    }
    if (m.ble_connected != g_ap.conn) {
        lv_obj_set_style_bg_color(g_ble_dot,
            m.ble_connected ? C_GREEN : C_DOT_IDLE, 0);
        g_ap.conn = m.ble_connected;
    }

    const bool api_mode = (q.mode == kSnapshotModeApi);
    if ((api_mode ? 1 : 0) != g_ap.hud_mode) {
        g_ap.hud_mode = api_mode ? 1 : 0;
        auto setRowsHidden = [&](bool hide) {
            for (lv_obj_t* o : {g_row5_lbl, g_row5_pct, g_row5_bar, g_row5_sub,
                                g_row7_lbl, g_row7_pct, g_row7_bar, g_row7_sub}) {
                if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
                else      lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
            }
        };
        setRowsHidden(api_mode);
        if (api_mode) {
            lv_obj_remove_flag(g_api_cost, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(g_api_dur,  LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_api_cost, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_api_dur,  LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (api_mode) {
        char cost[16];
        snprintf(cost, sizeof(cost), "$%u.%02u",
                 (unsigned)(q.cost_micro_usd / 1000000u),
                 (unsigned)((q.cost_micro_usd % 1000000u) / 10000u));
        if (std::strcmp(cost, g_ap.cost) != 0) {
            lv_label_set_text(g_api_cost, cost);
            std::strcpy(g_ap.cost, cost);
        }
        char dur[24] = "";
        if (q.duration_s > 0) {
            char t[16]; fmtRemaining(q.duration_s, t, sizeof(t));
            snprintf(dur, sizeof(dur), "t: %s", t);
        }
        if (std::strcmp(dur, g_ap.dur) != 0) {
            lv_label_set_text(g_api_dur, dur);
            std::strcpy(g_ap.dur, dur);
        }
    } else {
        auto applyRow = [&](uint16_t used, uint16_t limit, uint32_t reset_cap,
                            lv_obj_t* pct_l, lv_obj_t* bar, lv_obj_t* sub,
                            int& ap_pct, char* ap_sub, size_t ap_sub_n) {
            const int pct = (limit == 0) ? 0 :
                static_cast<int>(
                    std::min<uint32_t>(100, (uint32_t)used * 100u / limit));
            if (pct != ap_pct) {
                lv_label_set_text_fmt(pct_l, "%d%%", pct);
                lv_obj_set_style_text_color(pct_l, tierColor(pct), 0);
                lv_bar_set_value(bar, pct, LV_ANIM_ON);
                lv_obj_set_style_bg_color(bar, tierColor(pct), LV_PART_INDICATOR);
                ap_pct = pct;
            }
            char rst[12];
            fmtRemaining(liveRemaining(reset_cap, q.last_update_ms, m.now_ms),
                         rst, sizeof(rst));
            char sub_txt[40];
            if (rst[0]) snprintf(sub_txt, sizeof(sub_txt), "%u / %u   %s",
                                  used, limit, rst);
            else        snprintf(sub_txt, sizeof(sub_txt), "%u / %u",
                                  used, limit);
            if (std::strncmp(sub_txt, ap_sub, ap_sub_n) != 0) {
                lv_label_set_text(sub, sub_txt);
                std::strncpy(ap_sub, sub_txt, ap_sub_n - 1);
            }
        };
        applyRow(q.used_5h, q.limit_5h, q.reset_in_s_5h,
                 g_row5_pct, g_row5_bar, g_row5_sub,
                 g_ap.pct5, g_ap.sub5, sizeof(g_ap.sub5));
        applyRow(q.used_7d, q.limit_7d, q.reset_in_s_7d,
                 g_row7_pct, g_row7_bar, g_row7_sub,
                 g_ap.pct7, g_ap.sub7, sizeof(g_ap.sub7));
    }

    // Footer freshness.
    char foot[40];
    bool red = false;
    if (q.last_update_ms == kTsUnset) {
        snprintf(foot, sizeof(foot), "waiting for data...");
    } else {
        const uint32_t mins = static_cast<uint32_t>(
            (m.now_ms >= q.last_update_ms ? m.now_ms - q.last_update_ms : 0)
            / 60000ULL);
        red = mins >= kStaleThresholdMin;
        if (mins == 0)      snprintf(foot, sizeof(foot), "updated <1m ago");
        else if (!red)      snprintf(foot, sizeof(foot), "updated %um ago",
                                      (unsigned)mins);
        else                snprintf(foot, sizeof(foot), "stale %um",
                                      (unsigned)mins);
    }
    if (std::strcmp(foot, g_ap.footer) != 0 || red != g_ap.footer_red) {
        lv_label_set_text(g_footer, foot);
        lv_obj_set_style_text_color(g_footer, red ? C_RED : C_MUTED, 0);
        std::strcpy(g_ap.footer, foot);
        g_ap.footer_red = red;
    }

    // State slot. Re-resolve when the state OR the tool name changes.
    static char s_last_detail[kAppStateDetailMaxLen + 1] = {0};
    const bool state_changed  = (m.app_state != g_ap.app_state);
    const bool detail_changed =
        (m.app_state == kAppStateTool) &&
        (std::strncmp(m.app_detail, s_last_detail,
                      kAppStateDetailMaxLen) != 0);
    if (state_changed || detail_changed) {
        setStateSlot(static_cast<int8_t>(m.app_state), m.app_detail);
        g_ap.app_state = static_cast<int8_t>(m.app_state);
        std::strncpy(s_last_detail, m.app_detail, sizeof(s_last_detail) - 1);
    }
}

}  // namespace cc_hud

#endif  // CCHUD_LVGL_UI
