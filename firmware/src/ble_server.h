// ble_server.h
// NimBLE GATT server for cc_hud.
//
// Exposes the well-known Service UUID with two characteristics:
//   * Quota  (write / write-no-response) — accepts the 9-byte structured
//     quota payload defined in config.h.
//   * State  (read / notify) — short ASCII status strings ("OK", "ERR len",
//     "ERR type") plus, on subscribe, a one-shot "READY".
//
// The server is callback-driven: when a valid quota write arrives, the
// caller-supplied `QuotaWriteHandler` is invoked from the BLE task. The
// handler is expected to be cheap (update an in-RAM snapshot, persist to
// NVS, mark a redraw flag).

#pragma once

#include <Arduino.h>

#include <functional>

#include "persistence.h"

namespace cc_hud {

using QuotaWriteHandler        = std::function<void(const QuotaSnapshot& parsed)>;
using ConnectionChangeHandler  = std::function<void(bool connected)>;

// Initialise NimBLE, build the GATT layout, and start advertising. Must be
// called from setup() exactly once.
void bleServerInit(const QuotaWriteHandler&       on_write,
                   const ConnectionChangeHandler& on_conn);

// True while at least one central is connected.
bool bleIsConnected();

// Push a short ASCII status string to the State characteristic via notify.
// Safe to call from any task. Strings longer than 20 chars are truncated.
void bleNotifyState(const char* msg);

}  // namespace cc_hud
