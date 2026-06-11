// main.cpp
// Top-level firmware for cc_hud. Glues persistence, display, and the BLE
// GATT server into a small state machine:
//
//   setup():    init Serial, init NVS, load last quota, init ST7789, init BLE
//   loop():     drain redraw / connection events, refresh footer every 5 s
//
// BLE callbacks fire on the NimBLE task; they push parsed state into
// `g_quota` / `g_conn_*` under a portMUX critical section, then set flags
// for the main task to consume. Display rendering only happens in loop().

#include <Arduino.h>

#include <cstring>

#include "ble_server.h"
#include "config.h"
#include "display.h"
#include "lvgl_spike.h"
#include "ota_server.h"
#include "persistence.h"

namespace cc_hud {

// Shared state between BLE callbacks (NimBLE task) and the Arduino loop task.
portMUX_TYPE  g_lock = portMUX_INITIALIZER_UNLOCKED;
QuotaSnapshot g_quota;

volatile bool g_redraw_pending = true;
volatile bool g_conn_pending   = false;
volatile bool g_conn_state     = false;
volatile bool g_alert_pending  = false;  // set true when a 5h/7d crossing >= 95% fired
volatile bool g_force_idle     = false;  // explicit force-idle latch (msg_type 0x05)
volatile int8_t g_force_mood   = kPetMoodAuto;  // -1 = compute from quota; else 0..4
volatile uint32_t g_thinking_until_ms = 0;  // millis() until which we draw the star

constexpr uint32_t kThinkingHoldMs = 90UL * 1000UL;  // 90s window after any quota push

// App-state, pushed by Claude Code hooks via msg_type 0x07.
volatile int8_t g_app_state = kAppStateUnset;
char            g_app_state_detail[kAppStateDetailMaxLen + 1] = {0};

uint32_t g_last_footer_tick = 0;

// Compute percent (mirrors the helper in display.cpp; keeping it local avoids
// pulling display internals into the BLE callback path).
static uint8_t computePct(uint16_t used, uint16_t limit) {
    if (limit == 0) return 0;
    const uint32_t pct = (static_cast<uint32_t>(used) * 100u) / limit;
    return pct > 100u ? 100u : static_cast<uint8_t>(pct);
}

// --------------------------------------------------------------- callbacks --
//
// Invoked from the NimBLE task. Keep the work minimal: stash the parsed
// snapshot, persist to NVS, flip the redraw flag — the main loop does the
// actual display work.
void onQuotaWrite(const QuotaSnapshot& parsed) {
    // Carry the alert flags and the idle-time fields forward from the
    // previous snapshot — a v1/v2/v3 payload only describes quota state,
    // so unix_ts / utc_offset_min / time_capture_ms / idle_status must
    // be preserved across writes (otherwise the idle screen would lose
    // its calibrated clock every time the statusline pushes quota).
    QuotaSnapshot next;
    {
        portENTER_CRITICAL(&g_lock);
        const bool      prev_alerted_5h    = g_quota.alerted_5h;
        const bool      prev_alerted_7d    = g_quota.alerted_7d;
        const uint32_t  prev_unix_ts       = g_quota.unix_ts;
        const int16_t   prev_utc_offset    = g_quota.utc_offset_min;
        const uint64_t  prev_time_capture  = g_quota.time_capture_ms;
        char            prev_idle_status[33];
        std::strncpy(prev_idle_status, g_quota.idle_status,
                     sizeof(prev_idle_status));
        prev_idle_status[sizeof(prev_idle_status) - 1] = '\0';
        portEXIT_CRITICAL(&g_lock);

        next = parsed;
        next.alerted_5h      = prev_alerted_5h;
        next.alerted_7d      = prev_alerted_7d;
        next.unix_ts         = prev_unix_ts;
        next.utc_offset_min  = prev_utc_offset;
        next.time_capture_ms = prev_time_capture;
        std::strncpy(next.idle_status, prev_idle_status,
                     sizeof(next.idle_status));
        next.idle_status[sizeof(next.idle_status) - 1] = '\0';
    }

    bool need_flash = false;
    if (next.mode == kSnapshotModeSubscription) {
        const uint8_t pct_5h = computePct(next.used_5h, next.limit_5h);
        const uint8_t pct_7d = computePct(next.used_7d, next.limit_7d);

        if (pct_5h < kAlertThresholdPct) next.alerted_5h = false;
        if (pct_7d < kAlertThresholdPct) next.alerted_7d = false;

        if (pct_5h >= kAlertThresholdPct && !next.alerted_5h) {
            next.alerted_5h = true;
            need_flash = true;
            Serial.printf("[ALERT] 5H crossed %u%% (now %u%%)\n",
                          (unsigned)kAlertThresholdPct, (unsigned)pct_5h);
        }
        if (pct_7d >= kAlertThresholdPct && !next.alerted_7d) {
            next.alerted_7d = true;
            need_flash = true;
            Serial.printf("[ALERT] 7D crossed %u%% (now %u%%)\n",
                          (unsigned)kAlertThresholdPct, (unsigned)pct_7d);
        }
    } else {
        // API mode has no notion of % usage — never alert.
        next.alerted_5h = false;
        next.alerted_7d = false;
    }

    portENTER_CRITICAL(&g_lock);
    g_quota          = next;
    g_redraw_pending = true;
    if (need_flash) g_alert_pending = true;
    // Any v1/v2/v3 quota push counts as "Claude is doing something".
    // Refresh the thinking-star window for kThinkingHoldMs from now.
    g_thinking_until_ms = millis() + kThinkingHoldMs;
    portEXIT_CRITICAL(&g_lock);

    if (persistenceSave(next)) {
        Serial.println("[NVS] saved");
    } else {
        Serial.println("[NVS] save FAILED");
    }
}

void onConnChange(bool connected) {
    portENTER_CRITICAL(&g_lock);
    g_conn_state   = connected;
    g_conn_pending = true;
    portEXIT_CRITICAL(&g_lock);
}

// v4 idle write: host pushed wall clock + status string. We merge only
// the idle fields into g_quota, leaving the quota data alone, and we
// deliberately do NOT update last_update_ms — that timer still counts
// from the last v1/v2/v3 quota write so the 30-min idle threshold
// behaves correctly.
void onIdleWrite(uint32_t unix_ts,
                 int16_t  utc_offset_min,
                 uint64_t capture_ms,
                 const char* idle_status) {
    // Sentinel: utc_offset_min == -32768 means "this is a force-idle
    // toggle command, not a normal time push". The unix_ts field carries
    // the command (1 = enter forced idle, 0 = leave).
    if (utc_offset_min == -32768) {
        portENTER_CRITICAL(&g_lock);
        g_force_idle = (unix_ts != 0);
        g_redraw_pending = true;
        portEXIT_CRITICAL(&g_lock);
        Serial.printf("[FORCE] g_force_idle = %d\n",
                      static_cast<int>(unix_ts != 0));
        return;
    }

    portENTER_CRITICAL(&g_lock);
    g_quota.unix_ts         = unix_ts;
    g_quota.utc_offset_min  = utc_offset_min;
    g_quota.time_capture_ms = capture_ms;
    std::strncpy(g_quota.idle_status, idle_status,
                 sizeof(g_quota.idle_status) - 1);
    g_quota.idle_status[sizeof(g_quota.idle_status) - 1] = '\0';
    g_redraw_pending = true;
    QuotaSnapshot snapshot_copy = g_quota;
    portEXIT_CRITICAL(&g_lock);

    if (persistenceSave(snapshot_copy)) {
        Serial.println("[NVS] idle saved");
    }
}

// v7 app-state write: Claude Code hook → host script → BLE → here.
// `state` is one of AppState (0..3). `detail` is the tool name when
// state == kAppStateTool, empty otherwise. We just stash it for the
// main loop's state tick to pick up.
void onStateWrite(int8_t state, const char* detail) {
    portENTER_CRITICAL(&g_lock);
    g_app_state = state;
    std::strncpy(g_app_state_detail, detail,
                 sizeof(g_app_state_detail) - 1);
    g_app_state_detail[sizeof(g_app_state_detail) - 1] = '\0';
    portEXIT_CRITICAL(&g_lock);
    Serial.printf("[STATE] %d (%s)\n",
                  static_cast<int>(state), detail);
}

}  // namespace cc_hud

// ============================================================== Arduino ===
void setup() {
    Serial.begin(115200);
    delay(2500);  // give USB CDC time to enumerate after reset
    Serial.println();
    Serial.println("[BOOT] cc_hud v1");
    Serial.printf("[CHIP] %s rev %d, %lu MHz, flash %lu MB, psram %lu B\n",
                  ESP.getChipModel(),
                  static_cast<int>(ESP.getChipRevision()),
                  static_cast<unsigned long>(ESP.getCpuFreqMHz()),
                  static_cast<unsigned long>(ESP.getFlashChipSize() /
                                             (1024UL * 1024UL)),
                  static_cast<unsigned long>(ESP.getPsramSize()));

    // Load last-known quota from NVS so we have something to show even
    // before the first BLE write arrives.
    cc_hud::QuotaSnapshot loaded;
    if (cc_hud::persistenceLoad(loaded)) {
        Serial.printf("[NVS] load ok: 5h %u/%u  7d %u/%u  ts %llu\n",
                      loaded.used_5h, loaded.limit_5h,
                      loaded.used_7d, loaded.limit_7d,
                      static_cast<unsigned long long>(loaded.last_update_ms));
    } else {
        Serial.println("[NVS] cold start (no quota saved)");
    }
    // millis() resets to 0 on boot, but NVS holds a stale last_update_ms
    // from the previous run. Reset the time-relative fields so the idle
    // timer counts from THIS boot and the clock waits for a fresh BLE
    // calibration before claiming to know the time.
    loaded.last_update_ms  = 0;
    loaded.time_capture_ms = 0;
    cc_hud::g_quota = loaded;

    // Bring up the display, paint first frame.
    cc_hud::displayInit();
    Serial.println("[TFT] init done");

#if CCHUD_LVGL_SPIKE
    // LVGL spike owns the screen — legacy renderer stays compiled but idle.
    cc_hud::lvglSpikeInit();
    Serial.println("[TFT] LVGL spike screen up");
#else
    cc_hud::DisplayView view;
    view.quota         = loaded;
    view.ble_connected = false;
    view.now_ms        = static_cast<uint64_t>(millis());
    cc_hud::displayRender(view, /*full_redraw=*/true);
    Serial.println("[TFT] initial render done");
#endif

    // Build BLE state in three steps so NimBLE's "all services must exist
    // before advertising starts" rule is honored:
    //   1. bleServerInit  — quota service + chars + advertising metadata
    //   2. otaServerInit  — OTA service + chars on the same server
    //   3. bleStartAdvertising — finally make us discoverable
    cc_hud::bleServerInit(cc_hud::onQuotaWrite,
                          cc_hud::onConnChange,
                          cc_hud::onIdleWrite,
                          cc_hud::onStateWrite);
    cc_hud::otaServerInit(cc_hud::bleGetServer());
    cc_hud::bleStartAdvertising();

    cc_hud::g_last_footer_tick = millis();
    Serial.println("[BOOT] setup done");
}

void loop() {
#if CCHUD_LVGL_SPIKE
    // Spike mode: LVGL drives the screen. BLE/OTA stay fully alive —
    // their callbacks only set flags we don't consume here, and the
    // OTA path draws its own screen directly (we stop ticking LVGL so
    // it can't overpaint the progress bar; device reboots after).
    if (!cc_hud::displayIsOtaActive()) {
        cc_hud::lvglSpikeTick();
    }
    delay(5);
    return;
#endif

    bool need_redraw = false;
    bool need_conn   = false;
    bool need_alert  = false;
    bool conn_state  = false;
    cc_hud::QuotaSnapshot snap;

    portENTER_CRITICAL(&cc_hud::g_lock);
    if (cc_hud::g_redraw_pending) {
        snap                       = cc_hud::g_quota;
        need_redraw                = true;
        cc_hud::g_redraw_pending   = false;
    }
    if (cc_hud::g_conn_pending) {
        conn_state                 = cc_hud::g_conn_state;
        need_conn                  = true;
        cc_hud::g_conn_pending     = false;
    }
    if (cc_hud::g_alert_pending) {
        need_alert                 = true;
        cc_hud::g_alert_pending    = false;
    }
    cc_hud::QuotaSnapshot snap_for_check = cc_hud::g_quota;
    portEXIT_CRITICAL(&cc_hud::g_lock);

    // Detect idle-mode transitions. We treat last_update_ms == 0 as
    // "boot moment", so even if no quota write has happened this boot
    // the device will fall to idle after kIdleThresholdMs ms of uptime.
    // A fresh quota write resets the timer.
    static bool s_was_idle = false;
    const uint64_t now_ms_check = static_cast<uint64_t>(millis());
    const uint64_t since_last =
        (now_ms_check > snap_for_check.last_update_ms)
            ? (now_ms_check - snap_for_check.last_update_ms)
            : 0;
    bool force_idle_now;
    portENTER_CRITICAL(&cc_hud::g_lock);
    force_idle_now = cc_hud::g_force_idle;
    portEXIT_CRITICAL(&cc_hud::g_lock);
    const bool now_idle = force_idle_now ||
                          (since_last > cc_hud::kIdleThresholdMs);
    if (now_idle != s_was_idle) {
        s_was_idle = now_idle;
        need_redraw = true;
        if (!need_redraw || snap.last_update_ms == 0) {
            snap = snap_for_check;
        }
        Serial.printf("[IDLE] mode flip -> %s\n", now_idle ? "idle" : "hud");
    }
    // Override the snapshot's mode to idle so displayRender picks the
    // idle branch (we never persist mode=2; it's a runtime decoration).
    if (now_idle) {
        snap.mode = cc_hud::kSnapshotModeIdle;
    }

    // Threshold alert first — it blocks ~5s and then forces a full redraw.
    if (need_alert) {
        Serial.println("[ALERT] flashing screen red 5s");
        cc_hud::displayFlashAlert();
        need_redraw = true;  // restore HUD content after the flash
        if (snap.last_update_ms == 0) {
            // If we never received any quota, fall back to current g_quota.
            portENTER_CRITICAL(&cc_hud::g_lock);
            snap = cc_hud::g_quota;
            portEXIT_CRITICAL(&cc_hud::g_lock);
        }
    }

    // When an OTA is in progress the OTA task owns the screen — skip all
    // HUD-side rendering until the device reboots into the new image.
    const bool ota_active = cc_hud::displayIsOtaActive();

    if (need_redraw && !ota_active) {
        cc_hud::DisplayView v;
        v.quota         = snap;
        v.ble_connected = conn_state || cc_hud::bleIsConnected();
        v.now_ms        = static_cast<uint64_t>(millis());
        cc_hud::displayRender(v, /*full_redraw=*/need_alert);
    }
    if (need_conn && !ota_active) {
        cc_hud::displayUpdateConnection(conn_state);
    }

    // Pet animation tick (idle only). Mood comes from current quota
    // state — at higher usage % the cat's expression and walking speed
    // change. Cheap: early-outs when nothing changed since last tick.
    static uint32_t s_last_pet_tick_ms = 0;
    const uint32_t now_pet = millis();
    if (!ota_active && s_was_idle &&
        now_pet - s_last_pet_tick_ms >= 16) {
        s_last_pet_tick_ms = now_pet;

        // Compute mood from the snapshot captured above (snap_for_check).
        const uint8_t pct_5h = (snap_for_check.limit_5h > 0)
            ? (uint8_t)((uint32_t)snap_for_check.used_5h * 100u /
                        snap_for_check.limit_5h) : 0;
        const uint8_t pct_7d = (snap_for_check.limit_7d > 0)
            ? (uint8_t)((uint32_t)snap_for_check.used_7d * 100u /
                        snap_for_check.limit_7d) : 0;
        const uint8_t pct = pct_5h > pct_7d ? pct_5h : pct_7d;

        cc_hud::PetMood mood;
        if (cc_hud::g_force_mood != cc_hud::kPetMoodAuto) {
            mood = static_cast<cc_hud::PetMood>(cc_hud::g_force_mood);
        } else if (pct >= 95)      mood = cc_hud::kPetMoodOverwhelmed;
        else if (pct >= 80)        mood = cc_hud::kPetMoodStressed;
        else if (pct >= 60)        mood = cc_hud::kPetMoodTired;
        else if (pct >= 30)        mood = cc_hud::kPetMoodCalm;
        else                       mood = cc_hud::kPetMoodHappy;

        cc_hud::displayTickPet(static_cast<uint64_t>(now_pet), mood);
    }

    // App-state tick (HUD mode only). Driven by g_app_state which is
    // set by msg_type 0x07 push from Claude Code hooks via host. ~30 ms
    // cadence = ~33 FPS, only redraws when something actually changes.
    // Idle/Stop falls back to the legacy "thinking" GIF for 90 s after
    // a quota write if no app state has ever been pushed (so devices
    // without hooks configured still show liveness).
    static uint32_t s_last_state_tick_ms = 0;
    if (!ota_active && !s_was_idle &&
        now_pet - s_last_state_tick_ms >= 30) {
        s_last_state_tick_ms = now_pet;

        cc_hud::AppState eff_state;
        char eff_detail[cc_hud::kAppStateDetailMaxLen + 1];
        portENTER_CRITICAL(&cc_hud::g_lock);
        const int8_t st_raw = cc_hud::g_app_state;
        std::strncpy(eff_detail, cc_hud::g_app_state_detail,
                     sizeof(eff_detail));
        eff_detail[sizeof(eff_detail) - 1] = '\0';
        const uint32_t until = cc_hud::g_thinking_until_ms;
        portEXIT_CRITICAL(&cc_hud::g_lock);

        if (st_raw == cc_hud::kAppStateUnset) {
            // No hook has ever pushed → use legacy thinking-window logic
            // so things still look alive without hook setup.
            const bool thinking = (until != 0) && (now_pet < until);
            eff_state = thinking ? cc_hud::kAppStateThinking
                                  : cc_hud::kAppStateIdle;
            eff_detail[0] = '\0';
        } else {
            eff_state = static_cast<cc_hud::AppState>(st_raw);
        }

        cc_hud::displayTickState(static_cast<uint64_t>(now_pet),
                                  eff_state, eff_detail);
    }

    // Footer freshness tick.
    const uint32_t now = millis();
    if (!ota_active &&
        now - cc_hud::g_last_footer_tick >= cc_hud::kFooterRefreshMs) {
        cc_hud::g_last_footer_tick = now;
        portENTER_CRITICAL(&cc_hud::g_lock);
        cc_hud::QuotaSnapshot s = cc_hud::g_quota;
        portEXIT_CRITICAL(&cc_hud::g_lock);
        if (s_was_idle) {
            // Idle mode: re-render so the clock catches the next minute.
            s.mode = cc_hud::kSnapshotModeIdle;
            cc_hud::DisplayView v;
            v.quota         = s;
            v.ble_connected = cc_hud::bleIsConnected();
            v.now_ms        = static_cast<uint64_t>(now);
            cc_hud::displayRender(v);
        } else {
            cc_hud::displayTickFooter(s, static_cast<uint64_t>(now));
        }
    }

    delay(50);  // light yield, keeps loop responsive without spinning hot
}
