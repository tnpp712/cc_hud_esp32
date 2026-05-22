// persistence.h
// Non-volatile storage helpers built on Arduino Preferences (NVS).
//
// QuotaSnapshot is the canonical in-memory form of "what we last knew about
// the quota". It is loaded once at boot from NVS, mutated when the BLE write
// handler validates a new payload, and persisted again immediately after.

#pragma once

#include <Arduino.h>

namespace cc_hud {

struct QuotaSnapshot {
    uint16_t used_5h   = 0;
    uint16_t limit_5h  = 0;
    uint16_t used_7d   = 0;
    uint16_t limit_7d  = 0;
    // Monotonic millis() captured at the moment of the last successful write.
    // 0 means "no quota has ever been received" — treat as a cold boot.
    uint64_t last_update_ms = 0;
};

// Open the cc_hud namespace and load whatever is on disk. Missing keys default
// to zero. Returns true if the load succeeded and at least one key was found
// (i.e. the device has been provisioned at least once before).
bool persistenceLoad(QuotaSnapshot& out);

// Persist the snapshot atomically. Returns true on success.
bool persistenceSave(const QuotaSnapshot& snap);

}  // namespace cc_hud
