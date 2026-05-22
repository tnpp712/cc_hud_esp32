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
#include "persistence.h"

namespace cc_hud {

// Shared state between BLE callbacks (NimBLE task) and the Arduino loop task.
portMUX_TYPE  g_lock = portMUX_INITIALIZER_UNLOCKED;
QuotaSnapshot g_quota;

volatile bool g_redraw_pending = true;
volatile bool g_conn_pending   = false;
volatile bool g_conn_state     = false;

uint32_t g_last_footer_tick = 0;

// --------------------------------------------------------------- callbacks --
//
// Invoked from the NimBLE task. Keep the work minimal: stash the parsed
// snapshot, persist to NVS, flip the redraw flag — the main loop does the
// actual display work.
void onQuotaWrite(const QuotaSnapshot& parsed) {
    portENTER_CRITICAL(&g_lock);
    g_quota          = parsed;
    g_redraw_pending = true;
    portEXIT_CRITICAL(&g_lock);

    if (persistenceSave(parsed)) {
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

    // Start the BLE GATT server and begin advertising.
    cc_hud::bleServerInit(cc_hud::onQuotaWrite, cc_hud::onConnChange);

    cc_hud::g_last_footer_tick = millis();
    Serial.println("[BOOT] setup done");
}

void loop() {
    bool need_redraw = false;
    bool need_conn   = false;
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
    portEXIT_CRITICAL(&cc_hud::g_lock);

    if (need_redraw) {
        cc_hud::DisplayView v;
        v.quota         = snap;
        v.ble_connected = conn_state || cc_hud::bleIsConnected();
        v.now_ms        = static_cast<uint64_t>(millis());
        cc_hud::displayRender(v);
    }
    if (need_conn) {
        cc_hud::displayUpdateConnection(conn_state);
    }

    // Footer freshness tick.
    const uint32_t now = millis();
    if (now - cc_hud::g_last_footer_tick >= cc_hud::kFooterRefreshMs) {
        cc_hud::g_last_footer_tick = now;
        portENTER_CRITICAL(&cc_hud::g_lock);
        cc_hud::QuotaSnapshot s = cc_hud::g_quota;
        portEXIT_CRITICAL(&cc_hud::g_lock);
        cc_hud::displayTickFooter(s, static_cast<uint64_t>(now));
    }

    delay(50);  // light yield, keeps loop responsive without spinning hot
}
