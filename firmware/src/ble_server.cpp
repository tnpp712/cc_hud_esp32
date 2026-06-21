// ble_server.cpp
// NimBLE GATT server implementation. The BLE stack runs on its own FreeRTOS
// task; callbacks below are invoked from that task, so the user-supplied
// handlers must avoid blocking and must protect their own shared state.

#include "ble_server.h"

#include <NimBLEDevice.h>

extern "C" {
#include <nimble/nimble/host/include/host/ble_gap.h>  // ble_gap_set_prefered_le_phy
}

#include <cstring>
#include <cstddef>

#include "config.h"

namespace cc_hud {

namespace {

// Cached handler pointers, set once by bleServerInit.
QuotaWriteHandler       g_on_write;
ConnectionChangeHandler g_on_conn;
IdleWriteHandler        g_on_idle;
StateWriteHandler       g_on_state;
WifiWriteHandler        g_on_wifi;

// Handles owned by the NimBLE stack. We keep raw pointers to push notifies.
NimBLEServer*         g_server      = nullptr;
NimBLECharacteristic* g_quota_char  = nullptr;
NimBLECharacteristic* g_state_char  = nullptr;

// Tracks whether at least one central is currently connected. Updated from
// the server callbacks, read by the rest of the firmware via bleIsConnected.
volatile bool g_connected = false;

// ----------------------------------------------------------------- helpers --
//
// Little-endian readers. Bounds validated by callers (we only enter the
// parse path after length+msg_type validation).
inline uint16_t leU16(const uint8_t* buf, size_t offset) {
    return static_cast<uint16_t>(buf[offset]) |
           (static_cast<uint16_t>(buf[offset + 1]) << 8);
}
inline uint32_t leU32(const uint8_t* buf, size_t offset) {
    return  static_cast<uint32_t>(buf[offset]) |
           (static_cast<uint32_t>(buf[offset + 1]) << 8) |
           (static_cast<uint32_t>(buf[offset + 2]) << 16) |
           (static_cast<uint32_t>(buf[offset + 3]) << 24);
}

// --------------------------------------------------------------- callbacks --
class QuotaCallbacks final : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* chr) override {
        const std::string& value = chr->getValue();
        const size_t        len   = value.length();
        const auto*         data  = reinterpret_cast<const uint8_t*>(value.data());

        Serial.printf("[BLE] write %u bytes", static_cast<unsigned>(len));
        if (len > 0) {
            Serial.printf(" msg_type=0x%02X", data[0]);
        }
        Serial.println();

        // Route by (length, msg_type).
        //   v1 (9 bytes, 0x01)  : used+limit only.
        //   v2 (17 bytes, 0x02) : v1 + 2× u32 reset_in_seconds.
        //   v3 (>=27 bytes, 0x03): v2 + mode + cost + duration + title.
        //   v4 (>=8 bytes, 0x04) : idle/time push (unix_ts + tz + status).
        const bool is_v1 = (len == kQuotaPayloadLenV1) &&
                           (data[0] == kQuotaMsgTypeV1);
        const bool is_v2 = (len == kQuotaPayloadLenV2) &&
                           (data[0] == kQuotaMsgTypeV2);
        bool is_v3 = false;
        size_t title_len = 0;
        if (len >= kQuotaPayloadLenV3Base && data[0] == kQuotaMsgTypeV3) {
            title_len = data[26];
            if (len == kQuotaPayloadLenV3Base + title_len && title_len <= kTitleMaxLen) {
                is_v3 = true;
            }
        }
        bool is_v5 = false;
        if (len >= kQuotaPayloadLenV5Base && data[0] == kQuotaMsgTypeV5) {
            title_len = data[27];
            if (len == kQuotaPayloadLenV5Base + title_len && title_len <= kTitleMaxLen) {
                is_v5 = true;
            }
        }
        bool is_v6 = false;
        if (len >= kQuotaPayloadLenV6Base && data[0] == kQuotaMsgTypeV6) {
            title_len = data[35];
            if (len == kQuotaPayloadLenV6Base + title_len && title_len <= kTitleMaxLen) {
                is_v6 = true;
            }
        }
        bool is_v4 = false;
        size_t v4_status_len = 0;
        if (len >= kQuotaPayloadLenV4Base && data[0] == kQuotaMsgTypeV4) {
            v4_status_len = data[7];
            if (len == kQuotaPayloadLenV4Base + v4_status_len &&
                v4_status_len <= kIdleStatusMaxLen) {
                is_v4 = true;
            }
        }

        // v4 path: idle/time push — dispatch and return.
        if (is_v4) {
            uint32_t unix_ts   = leU32(data, 1);
            int16_t  utc_off   = static_cast<int16_t>(leU16(data, 5));
            char     status[kIdleStatusMaxLen + 1] = {0};
            if (v4_status_len > 0) {
                std::memcpy(status, data + 8, v4_status_len);
            }
            status[v4_status_len] = '\0';
            Serial.printf("[BLE] idle write ts=%lu tz=%d status=%s\n",
                          static_cast<unsigned long>(unix_ts),
                          static_cast<int>(utc_off), status);
            if (g_on_idle) {
                g_on_idle(unix_ts, utc_off,
                          static_cast<uint64_t>(millis()), status);
            }
            bleNotifyState(kStateOk);
            return;
        }

        // App-state push (msg_type 0x07, ≥3 bytes).
        if (len >= 3 && data[0] == kQuotaMsgTypeState) {
            const uint8_t st = data[1];
            const uint8_t dl = data[2];
            if (len < 3 + dl) {
                bleNotifyState(kStateErrLen);
                return;
            }
            char detail[kAppStateDetailMaxLen + 1] = {0};
            const size_t copy_n =
                dl < kAppStateDetailMaxLen ? dl : kAppStateDetailMaxLen;
            if (copy_n > 0) std::memcpy(detail, data + 3, copy_n);
            detail[copy_n] = '\0';
            // Optional stage-3 session-count bytes after the detail string.
            uint8_t total_sessions = 0, busy_sessions = 0;
            if (len >= static_cast<size_t>(3 + dl + 2)) {
                total_sessions = data[3 + dl];
                busy_sessions  = data[3 + dl + 1];
            }
            Serial.printf("[BLE] state=%u detail=%s sess=%u/%u\n",
                          static_cast<unsigned>(st), detail,
                          static_cast<unsigned>(busy_sessions),
                          static_cast<unsigned>(total_sessions));
            if (g_on_state) g_on_state(static_cast<int8_t>(st), detail,
                                       total_sessions, busy_sessions);
            bleNotifyState(kStateOk);
            return;
        }

        // WiFi-credentials push (msg_type 0x09, ≥3 bytes):
        //   [0]=0x09 [1]=ssid_len [2..]=ssid [+]=pwd_len [+]=pwd
        if (len >= 3 && data[0] == kQuotaMsgTypeWifi) {
            const uint8_t ssid_len = data[1];
            if (ssid_len == 0 || ssid_len > kWifiSsidMaxLen ||
                len < static_cast<size_t>(2u + ssid_len + 1u)) {
                bleNotifyState(kStateErrLen);
                return;
            }
            const uint8_t pwd_len = data[2 + ssid_len];
            if (pwd_len > kWifiPwdMaxLen ||
                len != static_cast<size_t>(3u + ssid_len + pwd_len)) {
                bleNotifyState(kStateErrLen);
                return;
            }
            char ssid[kWifiSsidMaxLen + 1] = {0};
            char pwd [kWifiPwdMaxLen  + 1] = {0};
            std::memcpy(ssid, data + 2, ssid_len);
            ssid[ssid_len] = '\0';
            if (pwd_len > 0) {
                std::memcpy(pwd, data + 3 + ssid_len, pwd_len);
            }
            pwd[pwd_len] = '\0';
            // Don't log the password — only SSID and length.
            Serial.printf("[BLE] wifi creds: ssid=%s pwd_len=%u\n",
                          ssid, static_cast<unsigned>(pwd_len));
            if (g_on_wifi) g_on_wifi(ssid, pwd);
            bleNotifyState(kStateOk);
            return;
        }

        // Force-idle toggle (msg_type 0x05, 2 bytes total).
        if (len == 2 && data[0] == kQuotaMsgTypeForceIdle) {
            const bool enter_idle = (data[1] != 0);
            Serial.printf("[BLE] force-idle cmd: %s\n",
                          enter_idle ? "ENTER" : "LEAVE");
            // The force-idle command piggy-backs on the idle handler:
            // call it with unix_ts=0 / status="" to mean "don't touch
            // time fields", and set g_force_idle separately via a NULL-
            // pointer flag-only marker — see main.cpp's onIdleWrite.
            if (g_on_idle) {
                // Use a sentinel: utc_offset_min = -32768 means "this
                // is a force-idle command, not a time push". main.cpp
                // catches this and sets/clears g_force_idle accordingly.
                g_on_idle(enter_idle ? 1u : 0u,
                          /*utc_offset_min=*/-32768,
                          static_cast<uint64_t>(millis()),
                          /*status=*/"");
            }
            bleNotifyState(kStateOk);
            return;
        }

        if (!is_v1 && !is_v2 && !is_v3 && !is_v5 && !is_v6) {
            // Distinguish "wrong length" from "wrong msg_type" for the host.
            if (data[0] == kQuotaMsgTypeV1 || data[0] == kQuotaMsgTypeV2 ||
                data[0] == kQuotaMsgTypeV3 || data[0] == kQuotaMsgTypeV4 ||
                data[0] == kQuotaMsgTypeV5 || data[0] == kQuotaMsgTypeV6) {
                bleNotifyState(kStateErrLen);
            } else {
                bleNotifyState(kStateErrType);
            }
            return;
        }

        QuotaSnapshot parsed;

        if (is_v3 || is_v5 || is_v6) {
            parsed.mode           = data[1];
            parsed.used_5h        = leU16(data, 2);
            parsed.limit_5h       = leU16(data, 4);
            parsed.used_7d        = leU16(data, 6);
            parsed.limit_7d       = leU16(data, 8);
            parsed.reset_in_s_5h  = leU32(data, 10);
            parsed.reset_in_s_7d  = leU32(data, 14);
            parsed.cost_micro_usd = leU32(data, 18);
            parsed.duration_s     = leU32(data, 22);
            // v5 inserts ctx_pct at 26 (title @28). v6 additionally inserts
            // two u32 line counters at 27..34 (title @36).
            size_t title_off = 27;
            if (is_v5) { parsed.ctx_pct = data[26]; title_off = 28; }
            if (is_v6) {
                parsed.ctx_pct       = data[26];
                parsed.lines_added   = leU32(data, 27);
                parsed.lines_removed = leU32(data, 31);
                title_off            = 36;
            }
            // Copy ASCII title (already length-bounded above).
            if (title_len > 0) {
                std::memcpy(parsed.title, data + title_off, title_len);
            }
            parsed.title[title_len] = '\0';
        } else {
            // v1 / v2 layout. Default mode = subscription, default title.
            parsed.mode    = kSnapshotModeSubscription;
            std::strncpy(parsed.title, "CC HUD", sizeof(parsed.title) - 1);
            parsed.title[sizeof(parsed.title) - 1] = '\0';
            parsed.used_5h        = leU16(data, 1);
            parsed.limit_5h       = leU16(data, 3);
            parsed.used_7d        = leU16(data, 5);
            parsed.limit_7d       = leU16(data, 7);
            if (is_v2) {
                parsed.reset_in_s_5h = leU32(data, 9);
                parsed.reset_in_s_7d = leU32(data, 13);
            }
        }

        parsed.last_update_ms = static_cast<uint64_t>(millis());

        if (g_on_write) {
            g_on_write(parsed);
        }
        bleNotifyState(kStateOk);
    }
};

class ServerCallbacks final : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* server) override {
        g_connected = true;
        Serial.println("[BLE] connected");

        // Request BLE 5.0 LE 2M PHY: doubles raw PHY rate from 1 Mbps
        // to 2 Mbps. macOS / iOS hosts generally accept (BLE 5.0+).
        // Also ask for the maximum LL packet length (DLE) for fewer
        // per-packet overheads.
        if (server->getConnectedCount() > 0) {
            NimBLEConnInfo info = server->getPeerInfo(0);
            const uint16_t conn = info.getConnHandle();

            // Data Length Extension: 251 byte LL packets at 2120 µs air-time.
            int rc = ble_gap_set_data_len(conn, 251, 2120);
            Serial.printf("[BLE] set_data_len rc=%d\n", rc);

            // PHY upgrade: prefer 2M for both directions.
            rc = ble_gap_set_prefered_le_phy(conn,
                                              BLE_GAP_LE_PHY_2M_MASK,
                                              BLE_GAP_LE_PHY_2M_MASK,
                                              0);
            Serial.printf("[BLE] request 2M PHY rc=%d (0 = OK, peer may decline)\n", rc);
        }

        if (g_on_conn) {
            g_on_conn(true);
        }
    }

    void onDisconnect(NimBLEServer* server) override {
        (void)server;
        g_connected = false;
        Serial.println("[BLE] disconnected");
        if (g_on_conn) {
            g_on_conn(false);
        }
        // Resume advertising so the next host can pair without a reboot.
        NimBLEDevice::startAdvertising();
        Serial.println("[BLE] adv restarted");
    }
};

QuotaCallbacks  g_quota_cb;
ServerCallbacks g_server_cb;

}  // namespace

// ---------------------------------------------------------------- public API
void bleServerInit(const QuotaWriteHandler&       on_write,
                   const ConnectionChangeHandler& on_conn,
                   const IdleWriteHandler&        on_idle,
                   const StateWriteHandler&       on_state,
                   const WifiWriteHandler&        on_wifi) {
    g_on_write = on_write;
    g_on_conn  = on_conn;
    g_on_idle  = on_idle;
    g_on_state = on_state;
    g_on_wifi  = on_wifi;

    NimBLEDevice::init(kBleDeviceName);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // Just-Works pairing: bonding optional, no MITM, no IO capability.
    NimBLEDevice::setSecurityAuth(/*bonding=*/false,
                                  /*mitm=*/false,
                                  /*sc=*/true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(&g_server_cb);

    NimBLEService* service = g_server->createService(kBleServiceUuid);

    g_quota_char = service->createCharacteristic(
        kBleQuotaCharUuid,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    g_quota_char->setCallbacks(&g_quota_cb);

    g_state_char = service->createCharacteristic(
        kBleStateCharUuid,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_state_char->setValue("READY");

    service->start();

    // Configure advertising metadata, but DON'T start it yet — the OTA module
    // needs to register its service on the same GATT server first, and NimBLE
    // does not allow service definitions to be added once advertising is up.
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(kBleServiceUuid);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMinPreferred(0x12);

    Serial.println("[BLE] services registered, waiting for advertising start");
}

void bleStartAdvertising() {
    NimBLEDevice::startAdvertising();
    Serial.println("[BLE] adv started");
}

bool bleIsConnected() {
    return g_connected;
}

NimBLEServer* bleGetServer() {
    return g_server;
}

void bleNotifyState(const char* msg) {
    if (g_state_char == nullptr || msg == nullptr) {
        return;
    }
    // Clamp to a generous safe length so we never blow past MTU expectations
    // on a default 23-byte ATT MTU.
    constexpr size_t kMaxLen = 20;
    char buf[kMaxLen + 1];
    std::strncpy(buf, msg, kMaxLen);
    buf[kMaxLen] = '\0';

    g_state_char->setValue(reinterpret_cast<const uint8_t*>(buf), std::strlen(buf));
    g_state_char->notify();
}

}  // namespace cc_hud
