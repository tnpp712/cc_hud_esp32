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

#include "battery.h"
#include "ble_server.h"
#include "button.h"
#include "config.h"
#include "display.h"
#include "led_ring.h"
#include "lvgl_ui.h"
#include "ota_server.h"
#include "persistence.h"
#include "wifi_ota.h"

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
// Multi-session counts from the aggregating hook (stage 3). 0 = unknown.
volatile uint8_t g_total_sessions = 0;
volatile uint8_t g_busy_sessions  = 0;
// One-shot "an AI finished a turn" pulse request (state 0x04 = done).
volatile bool g_ping_pending = false;

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
    // NOTE: deliberately do NOT touch g_thinking_until_ms here. The app-state
    // (idle/thinking/tool) is driven solely by explicit state pushes
    // (msg_type 0x07) from the hooks. Coupling it to quota writes made the
    // statusline's ~30s quota cadence perpetually re-arm a 90s "thinking"
    // window, so the device looked stuck "thinking" forever once any quota
    // arrived (the legacy fallback below only fires while no 0x07 has ever
    // been seen — e.g. right after boot — but the quota cadence kept it lit).
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
void onStateWrite(int8_t state, const char* detail,
                  uint8_t total_sessions, uint8_t busy_sessions) {
    // state 0x04 (done) is a one-shot "turn finished" signal: fire the green
    // done-pulse and settle to idle rather than persisting a "done" state.
    if (state == kAppStateDone) {
        g_ping_pending = true;
        state = kAppStateIdle;
    }
    portENTER_CRITICAL(&g_lock);
    g_app_state = state;
    std::strncpy(g_app_state_detail, detail,
                 sizeof(g_app_state_detail) - 1);
    g_app_state_detail[sizeof(g_app_state_detail) - 1] = '\0';
    g_total_sessions = total_sessions;
    g_busy_sessions  = busy_sessions;
    portEXIT_CRITICAL(&g_lock);
    Serial.printf("[STATE] %d (%s) sess=%u/%u\n",
                  static_cast<int>(state), detail,
                  static_cast<unsigned>(busy_sessions),
                  static_cast<unsigned>(total_sessions));
}

// v9 WiFi credentials write — persist + reconnect. Safe to call from the
// NimBLE task; wifi_ota's NVS write is small/fast and WiFi.begin is
// non-blocking.
void onWifiWrite(const char* ssid, const char* password) {
    cc_hud::wifiOtaSetCredentials(ssid, password);
}

// ----------------------------------------------------- burn prediction --
// Tracks usage over time and exposes a smoothed burn rate so the HUD can
// warn "this window will hit 100% before it resets". Fed one fresh sample
// per quota push (~30 s cadence); EMA-smoothed to ride out the noise.
struct BurnTrack {
    bool     have       = false;
    uint64_t t_ms       = 0;
    uint16_t used       = 0;
    float    rate_per_s = 0.0f;   // EMA of used-units per second
};

void burnUpdate(BurnTrack& b, uint16_t used, uint64_t t_ms) {
    if (!b.have) { b.have = true; b.t_ms = t_ms; b.used = used; return; }
    if (t_ms <= b.t_ms) return;
    if (used < b.used) {            // window reset → restart cleanly
        b.t_ms = t_ms; b.used = used; b.rate_per_s = 0.0f; return;
    }
    const float dt = (t_ms - b.t_ms) / 1000.0f;
    if (dt < 1.0f) return;
    const float inst = (used - b.used) / dt;   // units/sec
    b.rate_per_s = (b.rate_per_s > 0.0f)
                       ? 0.6f * b.rate_per_s + 0.4f * inst
                       : inst;
    b.t_ms = t_ms;
    b.used = used;
}

// Projected seconds until `used` reaches `limit`. 0 = no usable prediction.
uint32_t burnEtaSeconds(const BurnTrack& b, uint16_t used, uint16_t limit) {
    if (b.rate_per_s <= 0.0001f || used >= limit) return 0;
    return static_cast<uint32_t>((limit - used) / b.rate_per_s);
}

// Seconds remaining until a window resets, decremented live from millis().
uint32_t resetRemaining(uint32_t captured_s, uint64_t cap_ms, uint64_t now_ms) {
    if (captured_s == 0) return 0;
    const uint32_t el = static_cast<uint32_t>(
        (now_ms >= cap_ms ? now_ms - cap_ms : 0) / 1000ULL);
    return el >= captured_s ? 0u : captured_s - el;
}

}  // namespace cc_hud

// ============================================================== Arduino ===
void setup() {
    // Bring up the panel FIRST, before USB CDC enumeration. With
    // ARDUINO_USB_CDC_ON_BOOT=1, plugging into an active USB host triggers
    // enumeration during the early boot window — host-side DTR/RTS toggles
    // and the USB interrupt storm can disturb ST7789 SPI init timing,
    // leaving the panel backlit but blank. Doing displayInit() before
    // Serial.begin() guarantees the SPI init sequence completes on a quiet
    // bus. (We lose the [TFT] log line — no CDC yet — but that's fine.)
    cc_hud::displayInit();
    cc_hud::lvglUiInit();

    // LED ring (WS2812B status light). Boots in Off mode; loop() drives
    // it from g_app_state once BLE hooks start arriving.
    cc_hud::ledRingInit();

    // WiFi + ArduinoOTA — no-op if NVS has no creds yet; otherwise connects
    // in the background and starts listening on cc-hud.local:3232.
    cc_hud::wifiOtaInit();

    // Optional add-ons — both ship safe when unwired (see config.h).
    cc_hud::batteryInit();
    cc_hud::buttonInit();

    Serial.begin(115200);
    // Only pause for USB-CDC enumeration if a host is actually attaching, so
    // early boot logs aren't lost when debugging over USB. `Serial` turns
    // truthy once the host asserts the CDC line, so we break out the instant
    // it's ready. On battery/charger no host ever attaches, so this falls
    // through after a short cap instead of the old fixed 2.5s stall — boot
    // feels instant off-USB. (Capped so a flaky CDC line can't hang startup.)
    for (uint32_t t0 = millis(); !Serial && (millis() - t0) < 600u; ) {
        delay(10);
    }
    Serial.println();
    Serial.println("[BOOT] cc_hud v1");
    Serial.println("[TFT] init done (pre-CDC)");
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

    // Build BLE state in three steps so NimBLE's "all services must exist
    // before advertising starts" rule is honored:
    //   1. bleServerInit  — quota service + chars + advertising metadata
    //   2. otaServerInit  — OTA service + chars on the same server
    //   3. bleStartAdvertising — finally make us discoverable
    cc_hud::bleServerInit(cc_hud::onQuotaWrite,
                          cc_hud::onConnChange,
                          cc_hud::onIdleWrite,
                          cc_hud::onStateWrite,
                          cc_hud::onWifiWrite);
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
    m.total_sessions = cc_hud::g_total_sessions;
    m.busy_sessions  = cc_hud::g_busy_sessions;
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

    // ── Burn-rate prediction: sample once per fresh quota push, then warn
    //    if either window is projected to exhaust before it resets. ──
    static cc_hud::BurnTrack s_burn5, s_burn7;
    static uint64_t s_last_sampled = 0;
    if (m.quota.last_update_ms != 0 &&
        m.quota.last_update_ms != s_last_sampled) {
        s_last_sampled = m.quota.last_update_ms;
        cc_hud::burnUpdate(s_burn5, m.quota.used_5h, m.quota.last_update_ms);
        cc_hud::burnUpdate(s_burn7, m.quota.used_7d, m.quota.last_update_ms);
    }
    {
        const uint32_t eta5 = cc_hud::burnEtaSeconds(
            s_burn5, m.quota.used_5h, m.quota.limit_5h);
        const uint32_t eta7 = cc_hud::burnEtaSeconds(
            s_burn7, m.quota.used_7d, m.quota.limit_7d);
        const uint32_t rst5 = cc_hud::resetRemaining(
            m.quota.reset_in_s_5h, m.quota.last_update_ms, now_ms);
        const uint32_t rst7 = cc_hud::resetRemaining(
            m.quota.reset_in_s_7d, m.quota.last_update_ms, now_ms);
        const bool warn5 = (eta5 > 0 && rst5 > 0 && eta5 < rst5);
        const bool warn7 = (eta7 > 0 && rst7 > 0 && eta7 < rst7);
        if (warn5 && (!warn7 || eta5 <= eta7)) {
            m.exhaust_warn = true; m.exhaust_which = 0; m.exhaust_eta_s = eta5;
        } else if (warn7) {
            m.exhaust_warn = true; m.exhaust_which = 1; m.exhaust_eta_s = eta7;
        }
    }

    // Optional battery monitor — sampled ~1 Hz internally. Fields default
    // to "no sensor" (255) until a divider is wired to kPinBatterySense.
    cc_hud::batteryTick(now_ms);
    m.battery_pct = cc_hud::batteryValid() ? cc_hud::batteryPercent() : 255;
    m.battery_low = cc_hud::batteryLow();

    // Optional button: short press = manual page advance, long press =
    // toggle manual dim. Unwired pin reads released (pull-up), so this is
    // dormant until a switch is connected to kPinButton.
    // Manual dim toggle (button long-press / web). Just flip the flag — the
    // unified dim block below applies it to BOTH the panel and the LED ring
    // (and won't fight the night/idle auto-dim).
    // Manual brightness override: -1 = auto (follow night/idle), 0 = force
    // bright, 1 = force dim. A long-press / web "dim" toggles between force-
    // dim and force-bright, and WINS over the auto night/idle dim (so you can
    // brighten the screen even while it's auto-dimmed in idle). The override
    // auto-clears when Claude becomes active again (see the dim block).
    static int8_t s_manual = -1;
    switch (cc_hud::buttonPoll(now_ms)) {
        case cc_hud::kBtnShort: cc_hud::lvglUiManualAdvance(); break;
        case cc_hud::kBtnLong:  s_manual = (s_manual == 1) ? 0 : 1; break;
        default: break;
    }
    // Web dashboard control — same actions, zero hardware (phone buttons).
    switch (cc_hud::wifiOtaPollCommand()) {
        case cc_hud::kWebCmdNextPage:  cc_hud::lvglUiManualAdvance(); break;
        case cc_hud::kWebCmdToggleDim: s_manual = (s_manual == 1) ? 0 : 1; break;
        default: break;
    }

    cc_hud::lvglUiApply(m);
    if (alert_now) {
        cc_hud::lvglUiFlashAlert();
    }
    cc_hud::lvglUiTick();

    // Set by the power-saver block below; read by the LED block after it.
    bool g_ring_off = false;

    // ── Night auto-dim: between 23:00 and 07:00 local, dim the panel and
    //    the LED ring. Only acts once the clock has been calibrated via
    //    BLE (unix_ts != 0). Guarded by static so we only write on change.
    {
        int hour = -1;
        if (m.quota.unix_ts != 0) {
            const uint32_t el =
                (m.quota.time_capture_ms > 0 && now_ms >= m.quota.time_capture_ms)
                    ? static_cast<uint32_t>(
                          (now_ms - m.quota.time_capture_ms) / 1000ULL)
                    : 0;
            time_t local = static_cast<time_t>(m.quota.unix_ts) + el +
                           static_cast<time_t>(m.quota.utc_offset_min) * 60;
            struct tm tmv;
            gmtime_r(&local, &tmv);
            hour = tmv.tm_hour;
        }
        const bool night = (hour >= 23 || (hour >= 0 && hour < 7));

        // Power saver: track how long since any Claude activity. Any activity
        // restores everything AND clears the manual override (back to auto).
        static uint32_t s_last_active = 0;
        static bool     s_was_active  = false;
        const bool active_now = (m.app_state == cc_hud::kAppStateTool ||
                                 m.app_state == cc_hud::kAppStateThinking ||
                                 m.app_state == cc_hud::kAppStateWaiting);
        if (active_now) {
            s_last_active = static_cast<uint32_t>(now_ms);
            // Clear the manual override only on the idle→active EDGE, not on
            // every tick. Resetting every loop while Claude keeps working wiped
            // a long-press dim on the very next iteration, so you could never
            // dim the screen while active. Edge-only keeps "auto-brighten when
            // work resumes" while letting a manual dim stick during work.
            if (!s_was_active) s_manual = -1;
        }
        s_was_active = active_now;
        const uint32_t idle_for =
            static_cast<uint32_t>(now_ms) - s_last_active;
        const bool long_idle = idle_for > cc_hud::kIdleDimMs;   // 3 min → dim
        g_ring_off = idle_for > cc_hud::kLedOffMs;               // 10 min → off

        // Dim decision: a manual override (long-press / web) WINS over the
        // auto night/idle dim — so you can brighten even while auto-dimmed.
        // Re-apply when it flips OR the web brightness slider changed.
        const bool    dim        = (s_manual >= 0)
                                       ? (s_manual == 1)
                                       : (night || long_idle);
        const uint8_t day_bright = cc_hud::wifiOtaUserBrightness();
        static int8_t  s_dim        = -1;
        static uint8_t s_last_bright = 255;
        if (static_cast<int8_t>(dim) != s_dim || day_bright != s_last_bright) {
            s_dim = static_cast<int8_t>(dim);
            s_last_bright = day_bright;
            cc_hud::displaySetBacklight(dim ? 25 : 100);
            cc_hud::ledRingSetBrightness(
                dim ? cc_hud::kLedRingBrightnessNight : day_bright);
        }
    }

    // One-shot done-pulse (e.g. Codex finished a turn). Owns the ring ~1.8s.
    if (cc_hud::g_ping_pending) {
        cc_hud::g_ping_pending = false;
        cc_hud::ledRingPulseDone();
    }

    // ── Status LED ring. Priority: low battery (orange alarm) > very-long
    //    idle (ring off to save power) > app-state. When idle/done, show the
    //    ambient quota gauge (tiered colour). ──
    if (m.battery_low) {
        cc_hud::ledRingSetMode(cc_hud::kLedModeBattLow);
    } else {
        const cc_hud::LedMode led_mode =
            cc_hud::ledRingModeForAppState(m.app_state);
        if (led_mode == cc_hud::kLedModeIdleDone) {
            if (g_ring_off) cc_hud::ledRingSetMode(cc_hud::kLedModeOff);
            else            cc_hud::ledRingShowGauge(pct);
        } else {
            cc_hud::ledRingSetMode(led_mode);
        }
    }
    cc_hud::ledRingTick();

    // Publish a snapshot to the WiFi dashboard (throttled to ~2 Hz — the
    // page polls every 3 s, so finer updates are wasted work).
    static uint32_t s_web_next = 0;
    if (static_cast<uint32_t>(now_ms) >= s_web_next) {
        s_web_next = static_cast<uint32_t>(now_ms) + 500;
        cc_hud::WebStatus ws;
        std::strncpy(ws.title, m.quota.title, sizeof(ws.title) - 1);
        ws.pct5 = pct_5h; ws.pct7 = pct_7d; ws.ctx = m.quota.ctx_pct;
        ws.cost_micro = m.quota.cost_micro_usd;
        ws.duration_s = m.quota.duration_s;
        ws.lines_added = m.quota.lines_added;
        ws.lines_removed = m.quota.lines_removed;
        ws.total_sessions = m.total_sessions;
        ws.busy_sessions  = m.busy_sessions;
        ws.app_state = static_cast<int8_t>(m.app_state);
        std::strncpy(ws.app_detail, m.app_detail, sizeof(ws.app_detail) - 1);
        ws.ble_connected = m.ble_connected;
        ws.exhaust_warn = m.exhaust_warn;
        ws.exhaust_which = m.exhaust_which;
        ws.exhaust_eta_s = m.exhaust_eta_s;
        ws.battery_pct = m.battery_pct;
        ws.battery_low = m.battery_low;
        cc_hud::wifiOtaSetStatus(ws);
    }

    // ── NTP auto time-sync. Once WiFi is up, configure NTP (UTC); when the
    //    system clock becomes valid, feed it into the snapshot so the clock
    //    page works and survives a power loss WITHOUT a manual BLE
    //    re-calibration. Re-checks once a minute to correct millis() drift. ──
    static bool     s_ntp_started   = false;
    static uint32_t s_ntp_next      = 0;
    if (cc_hud::wifiOtaReady()) {
        if (!s_ntp_started) {
            configTime(0, 0, cc_hud::kNtpServer1, cc_hud::kNtpServer2);  // UTC
            s_ntp_started = true;
        }
        if (static_cast<uint32_t>(now_ms) >= s_ntp_next) {
            s_ntp_next = static_cast<uint32_t>(now_ms) + 60000;
            const time_t t = time(nullptr);
            if (t > 1700000000) {   // NTP responded (epoch after 2023)
                portENTER_CRITICAL(&cc_hud::g_lock);
                cc_hud::g_quota.unix_ts         = static_cast<uint32_t>(t);
                cc_hud::g_quota.time_capture_ms = static_cast<uint64_t>(now_ms);
                if (cc_hud::g_quota.utc_offset_min == 0)
                    cc_hud::g_quota.utc_offset_min = cc_hud::kDefaultTzMin;
                portEXIT_CRITICAL(&cc_hud::g_lock);
            }
        }
    }

    // Service WiFi ArduinoOTA + dashboard — no-op when WiFi isn't up yet.
    cc_hud::wifiOtaTick();

    delay(5);
}
