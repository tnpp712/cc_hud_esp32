// ota_server.cpp
// BLE OTA implementation. See ota_server.h for the protocol.
//
// The arduino-esp32 `Update` library does all the heavy lifting: pick the
// inactive OTA slot, write the new image, validate the magic byte at end,
// switch the otadata partition atomically. We just shuttle BLE-write bytes
// into `Update.write(...)` and call `Update.end(true)` on END.

#include "ota_server.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Update.h>

#include <cstdio>
#include <cstring>

#include "display.h"

namespace cc_hud {

namespace {

// Distinct UUID prefix from the quota service (ends in ...bb) so clients can
// distinguish them in a single scan.
constexpr const char* kOtaServiceUuid   = "12345678-aaaa-bbbb-cccc-1234567890bb";
constexpr const char* kOtaCtrlCharUuid  = "12345678-aaaa-bbbb-cccc-1234567890b1";
constexpr const char* kOtaDataCharUuid  = "12345678-aaaa-bbbb-cccc-1234567890b2";
constexpr const char* kOtaStateCharUuid = "12345678-aaaa-bbbb-cccc-1234567890b3";

constexpr uint8_t kCmdStart = 0x00;
constexpr uint8_t kCmdEnd   = 0x01;
constexpr uint8_t kCmdAbort = 0x02;

// Progress notify cadence — emit one ASCII line per ~8 KB written so the
// host can show a progress bar without saturating notify queue.
constexpr uint32_t kProgressIntervalBytes = 8192;

NimBLECharacteristic* g_ctrl  = nullptr;
NimBLECharacteristic* g_data  = nullptr;
NimBLECharacteristic* g_state = nullptr;

bool     g_ota_active            = false;
uint32_t g_expected_size         = 0;
uint32_t g_received              = 0;
uint32_t g_last_progress_notify  = 0;

void notifyState(const char* msg) {
    if (g_state == nullptr || msg == nullptr) return;
    g_state->setValue(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));
    g_state->notify();
    Serial.printf("[OTA] -> %s\n", msg);
}

class CtrlCallbacks final : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* c) override {
        const std::string& v = c->getValue();
        if (v.empty()) { notifyState("ERR empty"); return; }
        const uint8_t  cmd  = static_cast<uint8_t>(v[0]);
        const auto*    raw  = reinterpret_cast<const uint8_t*>(v.data());
        const size_t   len  = v.length();

        if (cmd == kCmdStart) {
            if (len < 5) { notifyState("ERR start len"); return; }
            const uint32_t size = static_cast<uint32_t>(raw[1])        |
                                  (static_cast<uint32_t>(raw[2]) << 8) |
                                  (static_cast<uint32_t>(raw[3]) << 16) |
                                  (static_cast<uint32_t>(raw[4]) << 24);
            Serial.printf("[OTA] START size=%u\n", static_cast<unsigned>(size));
            if (!Update.begin(size, U_FLASH)) {
                Serial.printf("[OTA] Update.begin failed: %s\n",
                              Update.errorString());
                notifyState("ERR begin");
                return;
            }
            g_expected_size        = size;
            g_received             = 0;
            g_last_progress_notify = 0;
            g_ota_active           = true;
            // Take over the screen with a full-screen OTA progress display.
            displayBeginOta();
            notifyState("READY");
        } else if (cmd == kCmdEnd) {
            if (!g_ota_active) { notifyState("ERR not started"); return; }
            Serial.printf("[OTA] END received=%u expected=%u\n",
                          static_cast<unsigned>(g_received),
                          static_cast<unsigned>(g_expected_size));
            if (!Update.end(true)) {
                Serial.printf("[OTA] Update.end failed: %s\n",
                              Update.errorString());
                notifyState("ERR end");
                g_ota_active = false;
                return;
            }
            notifyState("OK");
            // Brief delay so the OK notify has a chance to be delivered
            // before we yank ourselves out of the BLE stack.
            delay(500);
            Serial.println("[OTA] rebooting into new image");
            ESP.restart();
        } else if (cmd == kCmdAbort) {
            if (g_ota_active) {
                Update.abort();
                g_ota_active = false;
            }
            notifyState("ABORTED");
        } else {
            notifyState("ERR cmd");
        }
    }
};

class DataCallbacks final : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* c) override {
        if (!g_ota_active) return;
        const std::string& v = c->getValue();
        if (v.empty()) return;
        const auto*  raw   = reinterpret_cast<const uint8_t*>(v.data());
        const size_t len   = v.length();
        const size_t wrote = Update.write(const_cast<uint8_t*>(raw), len);
        if (wrote != len) {
            Serial.printf("[OTA] write short %u/%u (%s)\n",
                          static_cast<unsigned>(wrote),
                          static_cast<unsigned>(len),
                          Update.errorString());
            notifyState("ERR write");
            Update.abort();
            g_ota_active = false;
            return;
        }
        g_received += wrote;
        // Always nudge the on-screen progress (cheap — internal pct cache
        // suppresses the actual SPI traffic when the integer % is unchanged).
        displayOtaProgress(g_received, g_expected_size);
        if (g_received - g_last_progress_notify >= kProgressIntervalBytes ||
            g_received == g_expected_size) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "PROG %u",
                          static_cast<unsigned>(g_received));
            notifyState(buf);
            g_last_progress_notify = g_received;
        }
    }
};

CtrlCallbacks g_ctrl_cb;
DataCallbacks g_data_cb;

}  // namespace

void otaServerInit(NimBLEServer* server) {
    if (server == nullptr) return;

    // Must be called BEFORE bleStartAdvertising() — NimBLE cannot add new
    // service definitions once advertising is live.
    NimBLEService* svc = server->createService(kOtaServiceUuid);

    g_ctrl = svc->createCharacteristic(kOtaCtrlCharUuid,
                                       NIMBLE_PROPERTY::WRITE);
    g_ctrl->setCallbacks(&g_ctrl_cb);

    g_data = svc->createCharacteristic(kOtaDataCharUuid,
                                       NIMBLE_PROPERTY::WRITE |
                                       NIMBLE_PROPERTY::WRITE_NR);
    g_data->setCallbacks(&g_data_cb);

    g_state = svc->createCharacteristic(kOtaStateCharUuid,
                                        NIMBLE_PROPERTY::READ |
                                        NIMBLE_PROPERTY::NOTIFY);
    g_state->setValue("IDLE");

    svc->start();

    // Note: we deliberately do NOT add kOtaServiceUuid to the advertising
    // payload — together with the quota service UUID it overflows the
    // 31-byte BLE advertising packet limit, which silently kills the whole
    // advert on NimBLE. Clients still find the OTA service via the standard
    // GATT service-discovery step after they connect.
    Serial.println("[OTA] service registered");
}

}  // namespace cc_hud
