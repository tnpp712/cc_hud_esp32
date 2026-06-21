// wifi_ota.h
// WiFi + ArduinoOTA service for fast over-the-air firmware updates (10x
// faster than the BLE OTA path). WiFi credentials are pushed once over
// BLE (msg_type 0x09) and persisted in NVS — after that, the device
// auto-connects on every boot and listens for ArduinoOTA pushes on port
// 3232 at the mDNS name `cc-hud.local`.
//
// BLE OTA still works as a fallback when WiFi is unavailable / not yet
// provisioned. The two paths are mutually exclusive at upload time (both
// use Update.h) — they coexist fine at runtime.

#pragma once

#include <Arduino.h>

namespace cc_hud {

// Read WiFi credentials from NVS, connect (non-blocking) and start the
// ArduinoOTA listener. If no credentials are provisioned yet, this is a
// no-op and the function returns immediately — call wifiOtaSetCredentials
// later to bring WiFi up.
void wifiOtaInit();

// Pump ArduinoOTA.handle() if WiFi is up. Cheap when nothing is happening
// (a few microseconds). Call every loop iteration.
void wifiOtaTick();

// Persist new WiFi credentials to NVS and (re)connect. Safe to call from
// the BLE callback task. Empty SSID disables WiFi and clears the saved
// credentials. Returns true on success (NVS write OK).
bool wifiOtaSetCredentials(const char* ssid, const char* password);

// True after WiFi is associated AND ArduinoOTA has begun listening.
bool wifiOtaReady();

// Battery saver: disable (radio off, OTA+web stopped) or re-enable WiFi.
// main.cpp calls this from USB-presence detection.
void wifiOtaSetEnabled(bool enabled);

// Live snapshot published to the WiFi dashboard (http://cc-hud.local/).
// main.cpp fills one each loop; the HTTP handlers format it on demand.
struct WebStatus {
    char     title[33]   = {0};
    uint8_t  pct5        = 0;
    uint8_t  pct7        = 0;
    uint8_t  ctx         = 0;
    uint32_t cost_micro  = 0;
    uint32_t duration_s  = 0;
    uint32_t lines_added = 0;
    uint32_t lines_removed = 0;
    uint8_t  total_sessions = 0;
    uint8_t  busy_sessions  = 0;
    int8_t   app_state   = -1;
    char     app_detail[16] = {0};
    bool     ble_connected = false;
    bool     exhaust_warn  = false;
    uint8_t  exhaust_which = 0;
    uint32_t exhaust_eta_s = 0;
    uint8_t  battery_pct   = 255;   // 255 = no sensor
    bool     battery_low   = false;
};

// Publish the latest status for the web dashboard. Cheap (struct copy).
void wifiOtaSetStatus(const WebStatus& s);

// Web-dashboard control commands (zero-hardware alternative to a physical
// button). The HTTP handlers queue a command; main.cpp polls + clears it.
enum WebCommand : uint8_t {
    kWebCmdNone      = 0,
    kWebCmdNextPage  = 1,   // POST /next   → advance page
    kWebCmdToggleDim = 2,   // POST /dim    → toggle backlight dim
    kWebCmdSetBright = 3,   // POST /bright → user changed brightness
};
WebCommand wifiOtaPollCommand();

// User-set day LED brightness (web slider), persisted to NVS. main.cpp uses
// this as the non-dim ring brightness so it survives reboot without reflash.
uint8_t wifiOtaUserBrightness();

}  // namespace cc_hud
