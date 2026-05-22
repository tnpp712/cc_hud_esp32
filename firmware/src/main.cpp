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

#include "ble_server.h"
#include "config.h"
#include "display.h"
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
    // Carry the alert flags forward from the previous snapshot, then update
    // them based on the new percentages. A flag latches true when we cross
    // the threshold (we flash once), and clears when we drop back below.
    QuotaSnapshot next;
    {
        portENTER_CRITICAL(&g_lock);
        const bool prev_alerted_5h = g_quota.alerted_5h;
        const bool prev_alerted_7d = g_quota.alerted_7d;
        portEXIT_CRITICAL(&g_lock);

        next = parsed;
        next.alerted_5h = prev_alerted_5h;
        next.alerted_7d = prev_alerted_7d;
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
    cc_hud::g_quota = loaded;

    // Bring up the display, paint first frame.
    cc_hud::displayInit();
    Serial.println("[TFT] init done");

    cc_hud::DisplayView view;
    view.quota         = loaded;
    view.ble_connected = false;
    view.now_ms        = static_cast<uint64_t>(millis());
    cc_hud::displayRender(view, /*full_redraw=*/true);
    Serial.println("[TFT] initial render done");

    // Build BLE state in three steps so NimBLE's "all services must exist
    // before advertising starts" rule is honored:
    //   1. bleServerInit  — quota service + chars + advertising metadata
    //   2. otaServerInit  — OTA service + chars on the same server
    //   3. bleStartAdvertising — finally make us discoverable
    cc_hud::bleServerInit(cc_hud::onQuotaWrite, cc_hud::onConnChange);
    cc_hud::otaServerInit(cc_hud::bleGetServer());
    cc_hud::bleStartAdvertising();

    cc_hud::g_last_footer_tick = millis();
    Serial.println("[BOOT] setup done");
}

void loop() {
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
    portEXIT_CRITICAL(&cc_hud::g_lock);

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

    // Footer freshness tick.
    const uint32_t now = millis();
    if (!ota_active &&
        now - cc_hud::g_last_footer_tick >= cc_hud::kFooterRefreshMs) {
        cc_hud::g_last_footer_tick = now;
        portENTER_CRITICAL(&cc_hud::g_lock);
        cc_hud::QuotaSnapshot s = cc_hud::g_quota;
        portEXIT_CRITICAL(&cc_hud::g_lock);
        cc_hud::displayTickFooter(s, static_cast<uint64_t>(now));
    }

    delay(50);  // light yield, keeps loop responsive without spinning hot
}
