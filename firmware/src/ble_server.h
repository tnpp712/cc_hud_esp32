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
// v4 idle write: host pushes wall clock + status string, no quota fields.
using IdleWriteHandler = std::function<void(uint32_t unix_ts,
                                            int16_t  utc_offset_min,
                                            uint64_t capture_ms,
                                            const char* idle_status)>;
// v7 app-state write: host (Claude Code hooks → statusline) pushes the
// current app state. `state` is one of AppState (config.h). `detail` is
// an ASCII tool name when state == kAppStateTool, empty otherwise.
using StateWriteHandler = std::function<void(int8_t state, const char* detail)>;

// Initialise NimBLE, build the quota GATT service, and configure advertising
// metadata. DOES NOT start advertising — that's separated out so additional
// services (e.g. OTA) can be registered before advertising goes live.
// Call bleStartAdvertising() once all services have been added.
void bleServerInit(const QuotaWriteHandler&       on_write,
                   const ConnectionChangeHandler& on_conn,
                   const IdleWriteHandler&        on_idle,
                   const StateWriteHandler&       on_state);

// Begin BLE advertising. Must be called once, after every other service has
// been registered onto the server returned by bleGetServer().
void bleStartAdvertising();

// True while at least one central is connected.
bool bleIsConnected();

// Push a short ASCII status string to the State characteristic via notify.
// Safe to call from any task. Strings longer than 20 chars are truncated.
void bleNotifyState(const char* msg);

}  // namespace cc_hud

// Forward-declared in the global namespace to match NimBLE's headers.
class NimBLEServer;

namespace cc_hud {

// Returns the underlying NimBLE server pointer after bleServerInit() has run.
// Used by the OTA module to add a second GATT service onto the same server.
NimBLEServer* bleGetServer();

}  // namespace cc_hud
