// persistence.cpp
// Thin wrapper around the Arduino Preferences API. Keeps the namespace name
// and key strings centralized in config.h so the rest of the firmware never
// has to know we use NVS.

#include "persistence.h"

#include <Preferences.h>

#include <cstring>

#include "config.h"

namespace cc_hud {

namespace {

// Single shared Preferences handle. We open/close it inside each call so we
// never leak it across BLE callbacks; NVS is fast enough for our cadence.
Preferences g_prefs;

}  // namespace

bool persistenceLoad(QuotaSnapshot& out) {
    if (!g_prefs.begin(kNvsNamespace, /*readOnly=*/true)) {
        // Namespace did not exist yet — first boot. Caller will see zeros.
        return false;
    }

    const bool has_any = g_prefs.isKey(kNvsKey5hUsed) ||
                         g_prefs.isKey(kNvsKey5hLim)  ||
                         g_prefs.isKey(kNvsKey7dUsed) ||
                         g_prefs.isKey(kNvsKey7dLim);

    out.mode           = g_prefs.getUChar(kNvsKeyMode,     kSnapshotModeSubscription);
    out.used_5h        = g_prefs.getUShort(kNvsKey5hUsed,  0);
    out.limit_5h       = g_prefs.getUShort(kNvsKey5hLim,   0);
    out.used_7d        = g_prefs.getUShort(kNvsKey7dUsed,  0);
    out.limit_7d       = g_prefs.getUShort(kNvsKey7dLim,   0);
    out.reset_in_s_5h  = g_prefs.getULong(kNvsKey5hReset,  0);
    out.reset_in_s_7d  = g_prefs.getULong(kNvsKey7dReset,  0);
    out.cost_micro_usd = g_prefs.getULong(kNvsKeyCostU,    0);
    out.duration_s     = g_prefs.getULong(kNvsKeyDurS,     0);
    out.last_update_ms = g_prefs.getULong64(kNvsKeyTs,     kTsUnset);
    // Title — falls back to "CC HUD" if no key on first boot.
    String t = g_prefs.getString(kNvsKeyTitle, "CC HUD");
    std::strncpy(out.title, t.c_str(), sizeof(out.title) - 1);
    out.title[sizeof(out.title) - 1] = '\0';

    g_prefs.end();
    return has_any;
}

bool persistenceSave(const QuotaSnapshot& snap) {
    if (!g_prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
        return false;
    }

    bool ok = true;
    ok &= g_prefs.putUChar(kNvsKeyMode,     snap.mode)           == sizeof(uint8_t);
    ok &= g_prefs.putUShort(kNvsKey5hUsed,  snap.used_5h)        == sizeof(uint16_t);
    ok &= g_prefs.putUShort(kNvsKey5hLim,   snap.limit_5h)       == sizeof(uint16_t);
    ok &= g_prefs.putUShort(kNvsKey7dUsed,  snap.used_7d)        == sizeof(uint16_t);
    ok &= g_prefs.putUShort(kNvsKey7dLim,   snap.limit_7d)       == sizeof(uint16_t);
    ok &= g_prefs.putULong(kNvsKey5hReset,  snap.reset_in_s_5h)  == sizeof(uint32_t);
    ok &= g_prefs.putULong(kNvsKey7dReset,  snap.reset_in_s_7d)  == sizeof(uint32_t);
    ok &= g_prefs.putULong(kNvsKeyCostU,    snap.cost_micro_usd) == sizeof(uint32_t);
    ok &= g_prefs.putULong(kNvsKeyDurS,     snap.duration_s)     == sizeof(uint32_t);
    ok &= g_prefs.putULong64(kNvsKeyTs,     snap.last_update_ms) == sizeof(uint64_t);
    // putString returns the byte count written including the null terminator.
    ok &= g_prefs.putString(kNvsKeyTitle,   snap.title)          > 0;

    g_prefs.end();
    return ok;
}

}  // namespace cc_hud
