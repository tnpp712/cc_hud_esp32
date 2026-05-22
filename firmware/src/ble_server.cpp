// ble_server.cpp
// NimBLE GATT server implementation. The BLE stack runs on its own FreeRTOS
// task; callbacks below are invoked from that task, so the user-supplied
// handlers must avoid blocking and must protect their own shared state.

#include "ble_server.h"

#include <NimBLEDevice.h>

#include <cstring>
#include <cstddef>

#include "config.h"

namespace cc_hud {

namespace {

// Cached handler pointers, set once by bleServerInit.
QuotaWriteHandler       g_on_write;
ConnectionChangeHandler g_on_conn;

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

        if (!is_v1 && !is_v2 && !is_v3) {
            // Distinguish "wrong length" from "wrong msg_type" for the host.
            if (data[0] == kQuotaMsgTypeV1 || data[0] == kQuotaMsgTypeV2 ||
                data[0] == kQuotaMsgTypeV3) {
                bleNotifyState(kStateErrLen);
            } else {
                bleNotifyState(kStateErrType);
            }
            return;
        }

        QuotaSnapshot parsed;

        if (is_v3) {
            parsed.mode           = data[1];
            parsed.used_5h        = leU16(data, 2);
            parsed.limit_5h       = leU16(data, 4);
            parsed.used_7d        = leU16(data, 6);
            parsed.limit_7d       = leU16(data, 8);
            parsed.reset_in_s_5h  = leU32(data, 10);
            parsed.reset_in_s_7d  = leU32(data, 14);
            parsed.cost_micro_usd = leU32(data, 18);
            parsed.duration_s     = leU32(data, 22);
            // Copy ASCII title (already length-bounded above).
            if (title_len > 0) {
                std::memcpy(parsed.title, data + 27, title_len);
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
        (void)server;
        g_connected = true;
        Serial.println("[BLE] connected");
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
                   const ConnectionChangeHandler& on_conn) {
    g_on_write = on_write;
    g_on_conn  = on_conn;

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
