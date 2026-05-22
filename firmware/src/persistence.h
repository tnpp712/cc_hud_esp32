// persistence.h
// Non-volatile storage helpers built on Arduino Preferences (NVS).
//
// QuotaSnapshot is the canonical in-memory form of "what we last knew about
// the quota". It is loaded once at boot from NVS, mutated when the BLE write
// handler validates a new payload, and persisted again immediately after.

#pragma once

#include <Arduino.h>

namespace cc_hud {

// Display mode received in v3 payloads (and persisted in NVS).
//   0 = subscription (default): show 5h + 7d rows with reset countdowns.
//   1 = api: show cost (USD) and session duration instead of rate limits.
// Older v1/v2 payloads imply subscription mode.
constexpr uint8_t kSnapshotModeSubscription = 0;
constexpr uint8_t kSnapshotModeApi          = 1;

struct QuotaSnapshot {
    // Common.
    uint8_t  mode      = kSnapshotModeSubscription;
    char     title[33] = {0};  // ASCII, null-terminated. Default = "CC HUD".

    // Subscription-mode fields.
    uint16_t used_5h   = 0;
    uint16_t limit_5h  = 0;
    uint16_t used_7d   = 0;
    uint16_t limit_7d  = 0;
    // Seconds until the 5h / 7d windows reset, as reported by the host at
    // capture time. 0 == unknown (e.g. v1 payload had no countdown). The
    // firmware deducts elapsed time from these via millis() at render time
    // so the countdown stays live between BLE pushes.
    uint32_t reset_in_s_5h = 0;
    uint32_t reset_in_s_7d = 0;

    // API-mode fields.
    uint32_t cost_micro_usd = 0;  // 1_000_000 = $1.00
    uint32_t duration_s     = 0;  // session duration in seconds

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
