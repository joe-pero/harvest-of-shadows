#pragma once
// =============================================================================
// Always-on ArduinoOTA — mirrors props/scarecrow-narrator-a1s/src/ota.h.
// Burt's WiFi STA is already permanently associated (burt_link.cpp's
// wifiConnect(), needed for ESP-NOW), so no windowing is needed here either.
//
// Hostname: "burt-a1s"   ·   password: OTA_PASSWORD (shared, wifi_credentials.h)
// =============================================================================
#include <Arduino.h>

void ota_begin();     // call once in setup(), after burt_begin() brings WiFi up
void ota_tick();      // call every loop()
