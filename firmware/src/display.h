// display.h
// ST7789 panel bring-up + the OTA progress screen. All regular UI (HUD,
// idle clock, pet, state slot) renders through LVGL — see lvgl_ui.h.

#pragma once

#include <Arduino.h>

namespace cc_hud {

// Initialise the ST7789 panel (SPI bus, init sequence, backlight, 40 MHz
// clock). Must be called once during setup() before lvglUiInit().
void displayInit();

// Set backlight brightness 0..100 (%) via LEDC PWM. Used for night
// auto-dim. 100 = full, 0 = off.
void displaySetBacklight(uint8_t pct);

// Mark OTA mode active and paint the initial full-screen OTA frame. While
// OTA is in progress the main loop stops ticking LVGL so the OTA screen
// isn't overwritten. The device reboots on OTA end, so there's no explicit
// "leave OTA mode" — a power-cycle restores the normal UI.
void displayBeginOta();
// Update the on-screen OTA progress. Called from the BLE OTA task each
// time a chunk is written; cheap no-op when the integer percentage
// hasn't changed since the previous call.
void displayOtaProgress(uint32_t received, uint32_t total);
// Returns true while OTA-mode is active (between displayBeginOta() and the
// device's reboot). The main loop uses this to skip LVGL ticking.
bool displayIsOtaActive();

}  // namespace cc_hud

// Forward declaration so callers don't need the Adafruit headers.
class Adafruit_ST7789;

namespace cc_hud {

// Raw panel accessor — the LVGL flush callback (lvgl_ui.cpp) pushes
// rendered pixels through this. Valid after displayInit().
Adafruit_ST7789& displayGetTft();

}  // namespace cc_hud
