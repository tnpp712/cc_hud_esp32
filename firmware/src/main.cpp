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
#include "lvgl_ui.h"
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

    // LVGL owns the HUD + idle screens; display.cpp keeps only the
    // panel init and the OTA progress screen.
    cc_hud::lvglUiInit();
    Serial.println("[TFT] LVGL UI up");

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

    Serial.println("[BOOT] setup done");
}

void loop() {
    // main stays the data owner: drain shared state under the lock,
    // derive idle/mood/app-state, then hand one model to the UI. The OTA screen still runs
    // on the legacy direct-draw path — we just stop ticking LVGL so it
    // can't overpaint the progress bar (device reboots after OTA).
    if (cc_hud::displayIsOtaActive()) {
        delay(50);
        return;
    }

    cc_hud::LvglUiModel m;
    bool alert_now = false;
    int8_t app_state_raw;
    uint32_t thinking_until;
    int8_t force_mood;
    bool force_idle;

    portENTER_CRITICAL(&cc_hud::g_lock);
    m.quota = cc_hud::g_quota;
    cc_hud::g_redraw_pending = false;
    cc_hud::g_conn_pending   = false;
    if (cc_hud::g_alert_pending) {
        alert_now = true;
        cc_hud::g_alert_pending = false;
    }
    app_state_raw  = cc_hud::g_app_state;
    std::strncpy(m.app_detail, cc_hud::g_app_state_detail,
                 sizeof(m.app_detail) - 1);
    m.app_detail[sizeof(m.app_detail) - 1] = '\0';
    thinking_until = cc_hud::g_thinking_until_ms;
    force_mood     = cc_hud::g_force_mood;
    force_idle     = cc_hud::g_force_idle;
    portEXIT_CRITICAL(&cc_hud::g_lock);

    const uint64_t now_ms = static_cast<uint64_t>(millis());
    m.now_ms        = now_ms;
    m.ble_connected = cc_hud::bleIsConnected();

    // Idle detection — same rule as legacy.
    const uint64_t lv_since_last =
        (now_ms > m.quota.last_update_ms)
            ? (now_ms - m.quota.last_update_ms) : 0;
    m.idle_mode = force_idle || (lv_since_last > cc_hud::kIdleThresholdMs);

    // Mood from max(5h%, 7d%), unless forced.
    const uint8_t pct_5h = cc_hud::computePct(m.quota.used_5h, m.quota.limit_5h);
    const uint8_t pct_7d = cc_hud::computePct(m.quota.used_7d, m.quota.limit_7d);
    const uint8_t pct = pct_5h > pct_7d ? pct_5h : pct_7d;
    if (force_mood != cc_hud::kPetMoodAuto) {
        m.mood = static_cast<cc_hud::PetMood>(force_mood);
    } else if (pct >= 95) m.mood = cc_hud::kPetMoodOverwhelmed;
    else if (pct >= 80)   m.mood = cc_hud::kPetMoodStressed;
    else if (pct >= 60)   m.mood = cc_hud::kPetMoodTired;
    else if (pct >= 30)   m.mood = cc_hud::kPetMoodCalm;
    else                  m.mood = cc_hud::kPetMoodHappy;

    // App state with legacy thinking-window fallback for hosts that
    // never push msg_type 0x07.
    if (app_state_raw == cc_hud::kAppStateUnset) {
        const bool thinking =
            (thinking_until != 0) &&
            (static_cast<uint32_t>(now_ms) < thinking_until);
        m.app_state = thinking ? cc_hud::kAppStateThinking
                                : cc_hud::kAppStateIdle;
        m.app_detail[0] = '\0';
    } else {
        m.app_state = static_cast<cc_hud::AppState>(app_state_raw);
    }

    cc_hud::lvglUiApply(m);
    if (alert_now) {
        cc_hud::lvglUiFlashAlert();
    }
    cc_hud::lvglUiTick();
    delay(5);
}
