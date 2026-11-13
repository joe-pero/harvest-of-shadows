#pragma once
// =============================================================================
// Windowed OTA service — same pattern as spooky-tree (props/spooky-tree).
// =============================================================================
// Normal operation is ESP-NOW only, channel-locked, NO WiFi association. This
// node associates to the ch-11 AP and runs an ArduinoOTA server ONLY while an
// OTA window is open — opened by a CMD_OTA_MODE broadcast (its own or another
// controller's) or the bench "ota" serial command — then tears WiFi back down
// and restores the ESP-NOW channel lock when the window expires.
//
// Hostname: "halloween-touchpad-ota"   ·   password: OTA_PASSWORD (shared)
// =============================================================================
#include <Arduino.h>

void ota_service_start(uint32_t durationMin);   // open the window (default caller passes 15)
void ota_service_tick();                        // call every loop — handles OTA + expiry
bool ota_service_active();