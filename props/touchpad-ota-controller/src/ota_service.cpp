#include "ota_service.h"
#include "wifi_credentials.h"
#include "hud.h"
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_wifi.h>

static bool     s_active   = false;
static uint32_t s_untilMs  = 0;

static void ts() {
    uint32_t ms = millis();
    Serial.printf("[%02u:%02u.%03u] ", ms / 60000, (ms % 60000) / 1000, ms % 1000);
}

static void restoreChannelLock() {
    WiFi.disconnect(true);
    delay(100);
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

static void ota_service_stop() {
    ArduinoOTA.end();
    restoreChannelLock();
    s_active  = false;
    s_untilMs = 0;
    hud_set_enabled(true);
    ts(); Serial.println("[ota] service ended — ESP-NOW channel lock restored");
}

void ota_service_start(uint32_t durationMin) {
    if (s_active) { ts(); Serial.println("[ota] already active — window not extended"); return; }
    if (durationMin == 0) durationMin = 15;
    ts(); Serial.printf("[ota] service starting — %lu min window\n", (unsigned long)durationMin);

    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    ts(); Serial.print("[ota] WiFi connecting");
    uint32_t deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) { delay(500); Serial.print("."); }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        ts(); Serial.println("[ota] WiFi connect FAILED — aborting, staying on ESP-NOW");
        restoreChannelLock();
        return;
    }
    ts(); Serial.printf("[ota] WiFi up — IP %s  ch %d\n",
                        WiFi.localIP().toString().c_str(), WiFi.channel());

    ArduinoOTA.setHostname("halloween-touchpad-ota");
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        hud_set_enabled(false);                 // free the SPI bus + CPU for the transfer
        ts(); Serial.println("[ota] upload starting");
    });
    ArduinoOTA.onEnd([]() { ts(); Serial.println("[ota] upload complete — rebooting"); });
    ArduinoOTA.onError([](ota_error_t e) { ts(); Serial.printf("[ota] error %u\n", e); });
    ArduinoOTA.begin();

    s_active  = true;
    s_untilMs = millis() + durationMin * 60000UL;
    hud_set_enabled(false);                      // panel dark while the window is open
    ts(); Serial.printf("[ota] ready as 'halloween-touchpad-ota' — window closes in %lu min\n",
                        (unsigned long)durationMin);
}

void ota_service_tick() {
    if (!s_active) return;
    ArduinoOTA.handle();
    if ((int32_t)(millis() - s_untilMs) >= 0) ota_service_stop();
}

bool ota_service_active() { return s_active; }
