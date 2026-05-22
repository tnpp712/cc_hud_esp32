// main.cpp
// Top-level firmware for cc_hud. Glues persistence, display, and the BLE
// GATT server together via a small state machine:
//
//   setup():    init serial, init TFT, load last quota from NVS, init NimBLE,
//               start advertising, render the loaded quota.
//   loop():     every kFooterRefreshMs, refresh the footer (freshness clock)
//               and, if a BLE write set the redraw flag, re-render rows.
//
// All shared state lives at file scope (statically allocated). The BLE
// callbacks run on the NimBLE task; they only flip a couple of `volatile`
// flags and copy the parsed snapshot into the shared `g_quota` struct under
// a small critical section guarded by `portENTER_CRITICAL`.

#include <Arduino.h>

#include "ble_server.h"
#include "config.h"
#include "display.h"
#include "persistence.h"

using namespace cc_hud;

namespace {

// Authoritative in-memory quota snapshot. Mutated by the BLE write callback
// and read by loop()/display.
QuotaSnapshot g_quota;

// Critical-section guard for g_quota / g_pending_redraw. NimBLE runs on a
// different FreeRTOS task than loop(), so we must protect every access.
portMUX_TYPE g_state_mux = portMUX_INITIALIZER_UNLOCKED;

// Set by the BLE write callback; cleared by loop() once rendered.
volatile bool g_pending_redraw = true;

// Last BLE-connection state surfaced to the UI thread.
volatile bool g_pending_conn_state    = false;
volatile bool g_have_pending_conn_evt = false;

// Cadence tracker for the periodic footer refresh.
uint32_t g_last_footer_tick_ms = 0;

// ------------------------------------------------------------- BLE handlers
void onQuotaWrite(const QuotaSnapshot& parsed) {
    // Copy parsed snapshot into shared state under the mux, then persist.
    portENTER_CRITICAL(&g_state_mux);
    g_quota          = parsed;
    g_pending_redraw = true;
    portEXIT_CRITICAL(&g_state_mux);

    // Persistence runs outside the critical section (NVS may block briefly).
    if (persistenceSave(parsed)) {
        Serial.println("[NVS] saved");
    } else {
        Serial.println("[NVS] save FAILED");
    }
}

void onConnectionChange(bool connected) {
    portENTER_CRITICAL(&g_state_mux);
    g_pending_conn_state    = connected;
    g_have_pending_conn_evt = true;
    portEXIT_CRITICAL(&g_state_mux);
}

}  // namespace

// ---------------------------------------------------------------- arduino --
void setup() {
    Serial.begin(115200);
    // Give the USB-CDC serial a moment to enumerate on Nano ESP32 (not all
    // hosts open the port instantly).
    delay(200);

    displayInit();

    if (persistenceLoad(g_quota)) {
        Serial.println("[NVS] load ok");
    } else {
        Serial.println("[NVS] load empty (first boot)");
    }
    Serial.printf("[BOOT] last quota %u/%u %u/%u\n",
                  g_quota.used_5h,  g_quota.limit_5h,
                  g_quota.used_7d,  g_quota.limit_7d);

    bleServerInit(onQuotaWrite, onConnectionChange);

    // Force a full repaint of the freshly-booted view.
    DisplayView view;
    view.quota         = g_quota;
    view.ble_connected = false;
    view.now_ms        = static_cast<uint64_t>(millis());
    displayRender(view, /*full_redraw=*/true);

    g_last_footer_tick_ms = millis();
    g_pending_redraw      = false;
}

void loop() {
    // 1) Handle any pending connection-state change quickly (header dot).
    bool conn_evt_pending = false;
    bool conn_state       = false;
    portENTER_CRITICAL(&g_state_mux);
    if (g_have_pending_conn_evt) {
        conn_evt_pending        = true;
        conn_state              = g_pending_conn_state;
        g_have_pending_conn_evt = false;
    }
    portEXIT_CRITICAL(&g_state_mux);
    if (conn_evt_pending) {
        displayUpdateConnection(conn_state);
    }

    // 2) Handle a pending quota redraw (BLE write occurred).
    bool          do_redraw = false;
    QuotaSnapshot snapshot;
    portENTER_CRITICAL(&g_state_mux);
    if (g_pending_redraw) {
        snapshot         = g_quota;
        g_pending_redraw = false;
        do_redraw        = true;
    }
    portEXIT_CRITICAL(&g_state_mux);

    if (do_redraw) {
        DisplayView view;
        view.quota         = snapshot;
        view.ble_connected = bleIsConnected();
        view.now_ms        = static_cast<uint64_t>(millis());
        displayRender(view, /*full_redraw=*/false);
        g_last_footer_tick_ms = millis();
    }

    // 3) Periodic footer refresh (freshness clock).
    const uint32_t now = millis();
    if ((now - g_last_footer_tick_ms) >= kFooterRefreshMs) {
        g_last_footer_tick_ms = now;
        QuotaSnapshot footer_snap;
        portENTER_CRITICAL(&g_state_mux);
        footer_snap = g_quota;
        portEXIT_CRITICAL(&g_state_mux);
        displayTickFooter(footer_snap, static_cast<uint64_t>(now));
    }

    // Cooperative yield. NimBLE runs on its own task but we still want to
    // give FreeRTOS a chance to schedule other work.
    delay(20);
}
