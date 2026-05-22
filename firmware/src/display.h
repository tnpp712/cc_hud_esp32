// display.h
// ST7789 240x240 rendering for the cc_hud quota readout.
//
// The display module owns a TFT_eSPI instance and keeps a `LastDrawnState`
// snapshot of what is currently on the panel. `render(...)` computes diffs
// against that snapshot and only repaints regions that actually changed.
//
// No dynamic allocation happens after `init()`.

#pragma once

#include <Arduino.h>

#include "persistence.h"

namespace cc_hud {

// View-model handed to the renderer. Contains everything needed to draw a
// frame; the renderer never reads NVS or BLE state directly.
struct DisplayView {
    QuotaSnapshot quota;
    bool          ble_connected = false;
    uint64_t      now_ms        = 0;  // current millis(), passed in by caller
};

// Initialise the ST7789 panel. Must be called once during setup() *before*
// the first render call. Paints a full black frame.
void displayInit();

// Diff against `LastDrawnState` and repaint only the regions that changed.
// `full_redraw`, when true, forces every region to be repainted (e.g. after
// boot or a major theme change).
void displayRender(const DisplayView& view, bool full_redraw = false);

// Lightweight footer-only update used by the periodic loop tick. Recomputes
// the "updated Xm ago" / "stale Xm" text from the supplied `now_ms`/quota
// timestamp and redraws only the footer region.
void displayTickFooter(const QuotaSnapshot& quota, uint64_t now_ms);

// Update only the header dot to reflect a new connection state. Cheap; called
// from BLE callbacks.
void displayUpdateConnection(bool connected);

// One-shot red-flash alert. Blocks the calling task for
// `kAlertFlashCycles * 2 * kAlertFlashIntervalMs` (~5 s by default), flashing
// the whole screen between red and the normal background. After it returns,
// the caller must invoke `displayRender(view, /*full_redraw=*/true)` to
// restore the normal HUD content.
void displayFlashAlert();

// Mark OTA mode active and paint the initial full-screen OTA frame. While
// OTA is in progress, displayRender() / displayTickFooter() become no-ops
// so the OTA screen isn't overwritten. The device reboots on OTA end, so
// there's no explicit "leave OTA mode" — a power-cycle restores the HUD.
void displayBeginOta();
// Update the on-screen OTA progress. Called from the BLE OTA task each
// time a chunk is written; cheap on no-op when the integer percentage
// hasn't changed since the previous call.
void displayOtaProgress(uint32_t received, uint32_t total);
// Returns true while OTA-mode is active (between displayBeginOta() and the
// device's reboot). Callers may use this to skip their own renders.
bool displayIsOtaActive();

}  // namespace cc_hud
