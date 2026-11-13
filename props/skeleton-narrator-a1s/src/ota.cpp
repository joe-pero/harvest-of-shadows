#include "ota.h"
#include "wifi_credentials.h"
#include <ArduinoOTA.h>

static void ts() {
    uint32_t ms = millis();
    Serial.printf("[%02u:%02u.%03u] ", ms / 60000, (ms % 60000) / 1000, ms % 1000);
}

void ota_begin() {
    ArduinoOTA.setHostname("burt-a1s");
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        ts(); Serial.println(F("[ota] upload starting"));
    });
    ArduinoOTA.onEnd([]() {
        ts(); Serial.println(F("[ota] upload complete — rebooting"));
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        static uint8_t lastPct = 255;
        uint8_t pct = (uint8_t)((done * 100UL) / total);
        if (pct != lastPct) { ts(); Serial.printf("[ota] %u%%\n", pct); lastPct = pct; }
    });
    ArduinoOTA.onError([](ota_error_t e) {
        ts(); Serial.printf("[ota] error %u\n", e);
    });
    ArduinoOTA.begin();
    ts(); Serial.println(F("[ota] ready as 'burt-a1s'"));
}

void ota_tick() {
    ArduinoOTA.handle();
}
