// =============================================================================
// zombie-ir-lights BENCH SENDER (ESP32 DevKit), drives the ESP-01S node over
// ESP-NOW on ch 11 WITHOUT touching the live Zombie Container.
// =============================================================================
// Sends ONLY the 12-byte BenchFrame (include/bench_frame.h), which every fleet
// receiver drops. It deliberately cannot send a real Msg: no SEGMENT_START,
// no SET_THEME, nothing the relay/touchpad would act on.
//
// Serial (115200):
//   start [sec]   start the show clock, optionally jumped ahead
//                 (e.g. "start 68" = 5 s before the 1:13.40 red, "start 165" =
//                  5 s before the 2:49.86 reset)
//   end           like the touchpad's SEGMENT_END (e-stop)
//   off           like the Scarecrow's SHOW_STATE OFF
//   soak <min>    overnight soak: a full show every <min> minutes (0 = stop)
// Each is sent 3x, ~200 ms apart (fleet convention).
// =============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "wifi_credentials.h"
#include "bench_frame.h"

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void sendBench(BenchCmd cmd, uint32_t arg) {
    BenchFrame f = {};
    f.cmd = cmd; f.arg = arg;
    bench_seal(f);
    for (int i = 0; i < 3; i++) {
        esp_err_t e = esp_now_send(BCAST, (const uint8_t*)&f, sizeof(f));
        Serial.printf("[bench] cmd %u arg %lu -> %s\n", cmd, (unsigned long)arg,
                      e == ESP_OK ? "sent" : esp_err_to_name(e));
        if (i < 2) delay(200);
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    if (esp_now_init() != ESP_OK) { Serial.println("[bench] esp_now_init FAILED"); return; }
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, BCAST, 6);
    p.channel = WIFI_CHANNEL; p.encrypt = false;
    esp_now_add_peer(&p);
    Serial.printf("\n[bench] zombie-ir-lights bench sender ready, ch %u. "
                  "cmds: start [sec] | end | off\n", WIFI_CHANNEL);
}

static uint32_t s_soakEveryMs = 0, s_soakNextAt = 0, s_soakCount = 0;

void loop() {
    if (s_soakEveryMs && (int32_t)(millis() - s_soakNextAt) >= 0) {
        s_soakNextAt = millis() + s_soakEveryMs;
        Serial.printf("[bench] soak show #%lu\n", (unsigned long)++s_soakCount);
        sendBench(BENCH_START, 0);
    }
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.startsWith("start")) {
        uint32_t sec = line.length() > 5 ? line.substring(6).toInt() : 0;
        sendBench(BENCH_START, sec * 1000);
    }
    else if (line == "end") sendBench(BENCH_END, 0);
    else if (line == "off") sendBench(BENCH_OFF, 0);
    else if (line.startsWith("soak")) {
        s_soakEveryMs = (uint32_t)line.substring(5).toInt() * 60000UL;
        s_soakNextAt = millis();
        Serial.printf("[bench] soak every %lu min (0 = off)\n", (unsigned long)(s_soakEveryMs / 60000));
    }
    else if (line.length()) Serial.printf("[bench] ? '%s'\n", line.c_str());
}
