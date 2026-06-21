// wifi_ota.cpp — WiFi STA + ArduinoOTA service.
// See wifi_ota.h for usage. Credentials live in their own NVS namespace
// ("cc_wifi") so they survive a quota-NVS clear.

#include "wifi_ota.h"

#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "display.h"

namespace cc_hud {

namespace {

constexpr const char* kWifiNvsNamespace = "cc_wifi";
constexpr const char* kWifiNvsKeySsid   = "ssid";
constexpr const char* kWifiNvsKeyPwd    = "pwd";
constexpr const char* kOtaHostname      = "cc-hud";   // → cc-hud.local

bool g_ota_listening = false;   // true after ArduinoOTA.begin()

// ── WiFi dashboard ─────────────────────────────────────────────
WebServer  g_web(80);
bool       g_web_started = false;
WebStatus  g_status;             // latest snapshot from main.cpp
WebCommand g_web_cmd = kWebCmdNone;  // queued by HTTP handlers, polled by main

// User-set day brightness (web slider), persisted to NVS. main.cpp reads it
// via wifiOtaUserBrightness() so the level survives reboot without reflash.
constexpr const char* kBrightNvsKey = "bright";
uint8_t g_user_bright = kLedRingBrightness;

void loadBrightness() {
    Preferences p;
    if (p.begin(kWifiNvsNamespace, /*readOnly=*/true)) {
        g_user_bright = p.getUChar(kBrightNvsKey, kLedRingBrightness);
        p.end();
    }
}
void saveBrightness(uint8_t v) {
    g_user_bright = v;
    Preferences p;
    if (p.begin(kWifiNvsNamespace, /*readOnly=*/false)) {
        p.putUChar(kBrightNvsKey, v);
        p.end();
    }
}

const char* appStateName(int8_t s) {
    switch (s) {
        case 0:  return "idle";
        case 1:  return "thinking";
        case 2:  return "tool";
        case 3:  return "waiting";
        default: return "unknown";
    }
}

String statusJson() {
    char buf[640];
    snprintf(buf, sizeof(buf),
        "{\"title\":\"%s\",\"pct5\":%u,\"pct7\":%u,\"ctx\":%u,"
        "\"cost_usd\":%u.%06u,\"duration_s\":%u,"
        "\"lines_added\":%u,\"lines_removed\":%u,"
        "\"sessions\":%u,\"busy\":%u,"
        "\"state\":\"%s\",\"detail\":\"%s\",\"ble\":%s,"
        "\"exhaust_warn\":%s,\"exhaust_which\":\"%s\",\"exhaust_eta_s\":%u,"
        "\"battery_pct\":%d,\"battery_low\":%s,"
        "\"fw\":\"%s\",\"uptime_s\":%lu,\"free_heap\":%lu}",
        g_status.title, g_status.pct5, g_status.pct7, g_status.ctx,
        (unsigned)(g_status.cost_micro / 1000000u),
        (unsigned)(g_status.cost_micro % 1000000u),
        g_status.duration_s, g_status.lines_added, g_status.lines_removed,
        g_status.total_sessions, g_status.busy_sessions,
        appStateName(g_status.app_state), g_status.app_detail,
        g_status.ble_connected ? "true" : "false",
        g_status.exhaust_warn ? "true" : "false",
        g_status.exhaust_which == 1 ? "7d" : "5h",
        g_status.exhaust_eta_s,
        g_status.battery_pct == 255 ? -1 : (int)g_status.battery_pct,
        g_status.battery_low ? "true" : "false",
        kFirmwareVersion,
        (unsigned long)(millis() / 1000UL),
        (unsigned long)ESP.getFreeHeap());
    return String(buf);
}

void handleRoot() {
    // Minimal auto-refreshing dashboard. Pulls /status.json client-side so
    // the page itself is static and tiny.
    static const char kHtml[] PROGMEM =
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>CC-HUD</title><style>"
        "body{background:#111;color:#eee;font-family:-apple-system,system-ui,sans-serif;margin:0;padding:20px}"
        ".c{max-width:480px;margin:0 auto}h1{font-size:20px;margin:0 0 16px}"
        ".r{display:flex;justify-content:space-between;padding:10px 0;border-bottom:1px solid #333}"
        ".k{color:#888}.v{font-weight:600}.bar{height:8px;background:#222;border-radius:4px;overflow:hidden;margin-top:4px}"
        ".bar>i{display:block;height:100%%}.warn{color:#f55}"
        "</style></head><body><div class=c>"
        "<h1 id=title>CC-HUD</h1>"
        "<div class=r><span class=k>5h</span><span class=v id=p5>-</span></div>"
        "<div class=bar><i id=b5 style='width:0;background:#0f0'></i></div>"
        "<div class=r><span class=k>7d</span><span class=v id=p7>-</span></div>"
        "<div class=bar><i id=b7 style='width:0;background:#0f0'></i></div>"
        "<div class=r><span class=k>context</span><span class=v id=ctx>-</span></div>"
        "<div class=r><span class=k>session</span><span class=v id=cost>-</span></div>"
        "<div class=r><span class=k>lines</span><span class=v id=lines>-</span></div>"
        "<div class=r><span class=k>state</span><span class=v id=state>-</span></div>"
        "<div class=r><span class=k>sessions</span><span class=v id=sess>-</span></div>"
        "<div class=r id=warnrow style='display:none'><span class=k warn>warning</span><span class='v warn' id=warn>-</span></div>"
        "<div class=r id=battrow style='display:none'><span class=k>battery</span><span class=v id=batt>-</span></div>"
        "<div class=r><span class=k>brightness</span><span class=v><input id=br type=range min=1 max=60 value=10 oninput='setbr(this.value)' style='width:140px'></span></div>"
        "<div style='margin-top:18px;display:flex;gap:10px'>"
        "<button onclick='cmd(\"next\")' style='flex:1;padding:14px;font-size:16px;background:#333;color:#eee;border:0;border-radius:8px'>next page</button>"
        "<button onclick='cmd(\"dim\")' style='flex:1;padding:14px;font-size:16px;background:#333;color:#eee;border:0;border-radius:8px'>dim</button>"
        "</div>"
        "<div class=r style='margin-top:14px;font-size:11px'><span class=k id=fw>-</span><span class=k id=up>-</span></div>"
        "</div><script>"
        "function tier(p){return p<=60?'#0f0':p<=84?'#ff0':'#f55'}"
        "function fmtup(s){let h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h+'h'+m+'m'}"
        "async function cmd(c){try{await fetch('/'+c,{method:'POST'})}catch(e){}}"
        "let brt;function setbr(v){clearTimeout(brt);brt=setTimeout(()=>fetch('/bright?v='+v,{method:'POST'}),250)}"
        "async function u(){try{let r=await fetch('/status.json');let d=await r.json();"
        "title.textContent=d.title||'CC-HUD';"
        "p5.textContent=d.pct5+'%';b5.style.width=d.pct5+'%';b5.style.background=tier(d.pct5);"
        "p7.textContent=d.pct7+'%';b7.style.width=d.pct7+'%';b7.style.background=tier(d.pct7);"
        "ctx.textContent=d.ctx+'%';"
        "cost.textContent='$'+d.cost_usd.toFixed(2)+'  '+Math.round(d.duration_s/60)+'m';"
        "lines.textContent='+'+d.lines_added+' -'+d.lines_removed;"
        "state.textContent=d.state+(d.detail?(' ('+d.detail+')'):'');"
        "sess.textContent=d.sessions+' total, '+d.busy+' busy';"
        "if(d.exhaust_warn){warnrow.style.display='flex';warn.textContent=d.exhaust_which+' out in ~'+Math.round(d.exhaust_eta_s/60)+'m'}else{warnrow.style.display='none'}"
        "if(d.battery_pct>=0){battrow.style.display='flex';batt.textContent=d.battery_pct+'%'+(d.battery_low?' LOW':'');batt.style.color=d.battery_low?'#f55':'#eee'}else{battrow.style.display='none'}"
        "fw.textContent='fw '+(d.fw||'?');up.textContent='up '+fmtup(d.uptime_s||0)+' · heap '+Math.round((d.free_heap||0)/1024)+'k';"
        "}catch(e){}}"
        "u();setInterval(u,3000);"
        "</script></body></html>";
    g_web.send_P(200, "text/html", kHtml);
}

void handleStatusJson() {
    g_web.send(200, "application/json", statusJson());
}

void handleNext() {
    g_web_cmd = kWebCmdNextPage;
    g_web.send(200, "text/plain", "ok");
}

void handleDim() {
    g_web_cmd = kWebCmdToggleDim;
    g_web.send(200, "text/plain", "ok");
}

void handleBright() {
    // POST /bright?v=N  (N = 1..60). Persisted; main applies it as the day
    // brightness on the next dim re-evaluation.
    int v = g_web.arg("v").toInt();
    if (v < 1) v = 1;
    if (v > 60) v = 60;
    saveBrightness(static_cast<uint8_t>(v));
    g_web_cmd = kWebCmdSetBright;   // nudge main to re-apply brightness now
    g_web.send(200, "text/plain", "ok");
}

void startWebIfNeeded() {
    if (g_web_started) return;
    if (WiFi.status() != WL_CONNECTED) return;
    g_web.on("/", handleRoot);
    g_web.on("/status.json", handleStatusJson);
    g_web.on("/next",   HTTP_POST, handleNext);
    g_web.on("/dim",    HTTP_POST, handleDim);
    g_web.on("/bright", HTTP_POST, handleBright);
    g_web.begin();
    g_web_started = true;
    Serial.printf("[WEB] dashboard at http://%s.local/ (%s)\n",
                  kOtaHostname, WiFi.localIP().toString().c_str());
}

void startOtaIfNeeded() {
    if (g_ota_listening) return;
    if (WiFi.status() != WL_CONNECTED) return;

    ArduinoOTA.setHostname(kOtaHostname);
    // No password by default — LAN-only, low blast radius. Users who
    // want auth can add ArduinoOTA.setPassword("...") here.
    ArduinoOTA.onStart([]() {
        Serial.printf("[OTA-WIFI] start (%s)\n",
                      ArduinoOTA.getCommand() == U_FLASH ? "flash" : "spiffs");
        // Take over the panel with the OTA progress screen — same one the
        // BLE OTA path uses. The main loop stops ticking LVGL while this is
        // active; the device reboots on success so it clears naturally.
        displayBeginOta();
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("[OTA-WIFI] end, rebooting");
        displayOtaProgress(100, 100);
    });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int total) {
        displayOtaProgress(p, total);
        static unsigned last_pct = 255;
        const unsigned pct = (total > 0) ? (p * 100u) / total : 0;
        if (pct != last_pct && pct % 10 == 0) {
            Serial.printf("[OTA-WIFI] %u%%\n", pct);
            last_pct = pct;
        }
    });
    ArduinoOTA.onError([](ota_error_t err) {
        Serial.printf("[OTA-WIFI] error %u — rebooting to recover\n",
                      static_cast<unsigned>(err));
        // A failed WiFi OTA leaves the panel on the OTA screen with LVGL
        // stopped; reboot to cleanly restore the normal UI.
        delay(1500);
        ESP.restart();
    });
    ArduinoOTA.begin();
    g_ota_listening = true;
    Serial.printf("[OTA-WIFI] listening on %s.local @ %s\n",
                  kOtaHostname, WiFi.localIP().toString().c_str());
}

// Kick a non-blocking WiFi.begin(). Connection completes in the background;
// startOtaIfNeeded() will pick it up on the next tick once associated.
void beginConnect(const char* ssid, const char* pwd) {
    if (!ssid || !ssid[0]) return;
    Serial.printf("[WIFI] connecting to %s\n", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(kOtaHostname);
    WiFi.begin(ssid, pwd && pwd[0] ? pwd : nullptr);
}

// Suspended = WiFi radio off (battery mode). When suspended we skip all
// servicing and the radio stays down to save power.
bool g_suspended = false;

void connectFromNvs() {
    Preferences p;
    if (!p.begin(kWifiNvsNamespace, /*readOnly=*/true)) {
        Serial.println("[WIFI] no creds — skipping (push msg_type 0x09 to provision)");
        return;
    }
    String ssid = p.getString(kWifiNvsKeySsid, "");
    String pwd  = p.getString(kWifiNvsKeyPwd,  "");
    p.end();
    if (ssid.isEmpty()) {
        Serial.println("[WIFI] no creds — skipping");
        return;
    }
    beginConnect(ssid.c_str(), pwd.c_str());
}

}  // namespace

void wifiOtaInit() {
    loadBrightness();
    connectFromNvs();
}

uint8_t wifiOtaUserBrightness() {
    return g_user_bright;
}

void wifiOtaSetEnabled(bool en) {
    if (en) {
        if (!g_suspended) return;
        g_suspended = false;
        Serial.println("[WIFI] resume (USB present)");
        connectFromNvs();
    } else {
        if (g_suspended) return;
        g_suspended = true;
        Serial.println("[WIFI] suspend (battery mode — radio off)");
        if (g_ota_listening) { ArduinoOTA.end(); g_ota_listening = false; }
        if (g_web_started)   { g_web.stop();     g_web_started   = false; }
        WiFi.disconnect(/*wifioff=*/true);  // radio off
    }
}

void wifiOtaTick() {
    if (g_suspended) return;
    // Cheap: WiFi.status() is a single field read.
    if (WiFi.status() == WL_CONNECTED) {
        startOtaIfNeeded();
        startWebIfNeeded();
        if (g_ota_listening) {
            ArduinoOTA.handle();
        }
        if (g_web_started) {
            g_web.handleClient();
        }
    }
}

void wifiOtaSetStatus(const WebStatus& s) {
    g_status = s;
}

WebCommand wifiOtaPollCommand() {
    const WebCommand c = g_web_cmd;
    g_web_cmd = kWebCmdNone;
    return c;
}

bool wifiOtaSetCredentials(const char* ssid, const char* password) {
    Preferences p;
    if (!p.begin(kWifiNvsNamespace, /*readOnly=*/false)) {
        Serial.println("[WIFI] NVS open FAILED");
        return false;
    }
    const bool clearing = (!ssid || !ssid[0]);
    if (clearing) {
        p.clear();
        p.end();
        Serial.println("[WIFI] cleared creds");
        if (g_ota_listening) ArduinoOTA.end();
        WiFi.disconnect(/*wifioff=*/true);
        g_ota_listening = false;
        return true;
    }
    p.putString(kWifiNvsKeySsid, ssid);
    p.putString(kWifiNvsKeyPwd,  password ? password : "");
    p.end();
    Serial.printf("[WIFI] saved creds for %s\n", ssid);

    // Reconnect with new creds. ArduinoOTA stays bound to whatever WiFi
    // brings up; clear the listening flag so startOtaIfNeeded re-binds.
    if (g_ota_listening) {
        ArduinoOTA.end();
        g_ota_listening = false;
    }
    WiFi.disconnect(/*wifioff=*/false);
    beginConnect(ssid, password);
    return true;
}

bool wifiOtaReady() {
    return g_ota_listening && WiFi.status() == WL_CONNECTED;
}

}  // namespace cc_hud
