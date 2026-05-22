// persistence.cpp
// Thin wrapper around the Arduino Preferences API. Keeps the namespace name
// and key strings centralized in config.h so the rest of the firmware never
// has to know we use NVS.

#include "persistence.h"

#include <Preferences.h>

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

    out.used_5h        = g_prefs.getUShort(kNvsKey5hUsed, 0);
    out.limit_5h       = g_prefs.getUShort(kNvsKey5hLim,  0);
    out.used_7d        = g_prefs.getUShort(kNvsKey7dUsed, 0);
    out.limit_7d       = g_prefs.getUShort(kNvsKey7dLim,  0);
    out.last_update_ms = g_prefs.getULong64(kNvsKeyTs,    kTsUnset);

    g_prefs.end();
    return has_any;
}

bool persistenceSave(const QuotaSnapshot& snap) {
    if (!g_prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
        return false;
    }

    bool ok = true;
    ok &= g_prefs.putUShort(kNvsKey5hUsed, snap.used_5h) == sizeof(uint16_t);
    ok &= g_prefs.putUShort(kNvsKey5hLim,  snap.limit_5h) == sizeof(uint16_t);
    ok &= g_prefs.putUShort(kNvsKey7dUsed, snap.used_7d) == sizeof(uint16_t);
    ok &= g_prefs.putUShort(kNvsKey7dLim,  snap.limit_7d) == sizeof(uint16_t);
    ok &= g_prefs.putULong64(kNvsKeyTs,    snap.last_update_ms) == sizeof(uint64_t);

    g_prefs.end();
    return ok;
}

}  // namespace cc_hud
