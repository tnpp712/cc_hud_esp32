// ble_server.cpp
// NimBLE GATT server implementation. The BLE stack runs on its own FreeRTOS
// task; callbacks below are invoked from that task, so the user-supplied
// handlers must avoid blocking and must protect their own shared state.

#include "ble_server.h"

#include <NimBLEDevice.h>

#include <cstring>

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
// Read a little-endian u16 from `buf[offset]`. Bounds are validated by the
// caller (we only enter parse() with a kQuotaPayloadLen-byte buffer).
inline uint16_t leU16(const uint8_t* buf, size_t offset) {
    return static_cast<uint16_t>(buf[offset]) |
           (static_cast<uint16_t>(buf[offset + 1]) << 8);
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

        if (len != kQuotaPayloadLen) {
            bleNotifyState(kStateErrLen);
            return;
        }
        if (data[0] != kQuotaMsgType) {
            bleNotifyState(kStateErrType);
            return;
        }

        QuotaSnapshot parsed;
        parsed.used_5h        = leU16(data, 1);
        parsed.limit_5h       = leU16(data, 3);
        parsed.used_7d        = leU16(data, 5);
        parsed.limit_7d       = leU16(data, 7);
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

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(kBleServiceUuid);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMinPreferred(0x12);

    NimBLEDevice::startAdvertising();
    Serial.println("[BLE] adv started");
}

bool bleIsConnected() {
    return g_connected;
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
