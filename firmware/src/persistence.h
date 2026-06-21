// persistence.h
// Non-volatile storage helpers built on Arduino Preferences (NVS).
//
// QuotaSnapshot is the canonical in-memory form of "what we last knew about
// the quota". It is loaded once at boot from NVS, mutated when the BLE write
// handler validates a new payload, and persisted again immediately after.

#pragma once

#include <Arduino.h>

namespace cc_hud {

// Display mode received in v3 / v4 payloads (and persisted in NVS).
//   0 = subscription (default): show 5h + 7d rows with reset countdowns.
//   1 = api: show cost (USD) and session duration instead of rate limits.
//   2 = idle (automatic, not pushed by host): show clock + status text.
// Older v1/v2 payloads imply subscription mode.
constexpr uint8_t kSnapshotModeSubscription = 0;
constexpr uint8_t kSnapshotModeApi          = 1;
constexpr uint8_t kSnapshotModeIdle         = 2;

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

    // Context-window usage of the active Claude Code session, 0..100 (%).
    // Pushed in v5 payloads; 0 means "unknown / not reported" and the UI
    // simply omits it. Session-scoped, so it is NOT persisted to NVS.
    uint8_t  ctx_pct        = 0;

    // Session line counters from the Claude Code statusline cost block,
    // pushed in v6 payloads. Feed the session-stats screen. Like ctx_pct
    // these are session-scoped and NOT persisted to NVS.
    uint32_t lines_added    = 0;
    uint32_t lines_removed  = 0;

    // Idle-mode time + status fields (set by v4 BLE writes).
    uint32_t unix_ts        = 0;        // host-supplied UTC Unix time
    int16_t  utc_offset_min = 0;        // local timezone offset from UTC
    uint64_t time_capture_ms = 0;       // millis() when unix_ts was received
    char     idle_status[33] = {0};     // free-form ASCII (weather, etc.)

    // One-shot threshold alert state — true after we've already flashed for
    // this 95%+ crossing. Reset to false when the percentage drops below.
    bool alerted_5h = false;
    bool alerted_7d = false;

    // Monotonic millis() captured at the moment of the last successful quota
    // write (any of v1/v2/v3). The loop uses this to decide whether to enter
    // idle mode after ~30 minutes of inactivity. 0 = no quota ever received.
    uint64_t last_update_ms = 0;
};

// Open the cc_hud namespace and load whatever is on disk. Missing keys default
// to zero. Returns true if the load succeeded and at least one key was found
// (i.e. the device has been provisioned at least once before).
bool persistenceLoad(QuotaSnapshot& out);

// Persist the snapshot atomically. Returns true on success.
bool persistenceSave(const QuotaSnapshot& snap);

}  // namespace cc_hud
