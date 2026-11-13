#pragma once
// =============================================================================
// Always-on ArduinoOTA — Scarecrow's WiFi STA is already permanently
// associated (fleet.cpp, ch 11, needed for ESP-NOW + the MIDDLE HTTP fetch),
// unlike the touchpad's windowed OTA service (props/touchpad-ota-controller)
// which normally stays ESP-NOW-only and opens WiFi only on demand. No
// start/stop dance needed here — just bring OTA up once WiFi is up and
// service it every loop.
//
// Hostname: "scarecrow-a1s"   ·   password: OTA_PASSWORD (shared, wifi_credentials.h)
// =============================================================================
#include <Arduino.h>

void ota_begin();     // call once in setup(), after fleet_begin() brings WiFi up
void ota_tick();      // call every loop()
