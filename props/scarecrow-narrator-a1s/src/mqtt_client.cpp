#include "mqtt_client.h"
#include "NarratorBanter.h"
#include "wifi_credentials.h"
#include "ProjectConstants.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <string.h>

// Wire format (this project defines it — no real publisher exists yet, see
// docs/TASK-a1s-hold-segment.md Part B): theme/tone are the same lowercase
// strings pools.cpp's theme_str()/tone_str() already use for bucket names
// ("corn"/"rotld"/"pumpkins", "friendly"/"scary") — human-testable with
// mosquitto_pub and directly reusable for pools_bucket_name().
//   harvest/show/params (retained): {"session_id":1,"theme":"rotld","tone":"scary","cycle":1}
//   harvest/show/cue:               {"session_id":1,"phase":"preshow"}

#define MQTT_CLIENT_ID              "scarecrow_a1s"
#define MQTT_TOPIC_PARAMS           "harvest/show/params"
#define MQTT_TOPIC_CUE              "harvest/show/cue"
#define MQTT_TOPIC_LEADER_STATUS    "harvest/show/leader/status"
#define MQTT_RECONNECT_INTERVAL_MS  5000

static WiFiClient   s_wifiClient;
static PubSubClient s_mqtt(s_wifiClient);
static uint32_t     s_lastReconnectAttempt = 0;

static ShowParamsCallback s_paramsCb = nullptr;
static ShowCueCallback    s_cueCb    = nullptr;

static void ts() {
    uint32_t ms = millis();
    Serial.printf("[%02u:%02u.%03u] ", ms / 60000, (ms % 60000) / 1000, ms % 1000);
}

static ShowTheme parseTheme(const char* s) {
    if (!strcmp(s, "rotld"))    return THEME_ROTLD;
    if (!strcmp(s, "pumpkins")) return THEME_MUTANT_PUMPKINS;
    return THEME_CORN;
}

static ShowMode parseTone(const char* s) {
    if (!strcmp(s, "intense")) return MODE_INTENSE;
    if (!strcmp(s, "scary"))   return MODE_SCARY;
    return MODE_FRIENDLY;
}

static void onMqttMessage(char* topic, uint8_t* payload, unsigned int length) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        ts(); Serial.printf("[mqtt] bad JSON on '%s': %s\n", topic, err.c_str());
        return;
    }

    if (!strcmp(topic, MQTT_TOPIC_PARAMS)) {
        if (!s_paramsCb) return;
        ShowParams p;
        p.session_id = doc["session_id"] | 0UL;
        p.theme      = parseTheme(doc["theme"] | "corn");
        p.tone       = parseTone(doc["tone"] | "scary");
        p.cycle      = doc["cycle"] | 0;
        p.banter_pool  = -1;
        p.banter_idx   = 0;
        p.banter_lines = 0;
        p.encore       = doc["encore"] | false;
        p.middle       = doc["middle"].is<bool>() ? (doc["middle"].as<bool>() ? 1 : 0) : -1;
        if (doc["banter"].is<JsonObject>()) {
            const char* pool = doc["banter"]["pool"] | "";
            int id = banter_pool_id(pool);
            uint16_t idx = doc["banter"]["idx"] | 0;
            if (id < 0 || idx == 0) {
                ts(); Serial.printf("[mqtt] params: unusable banter pool='%s' idx=%u -- OUTRO pool fallback\n",
                                    pool, idx);
            } else {
                p.banter_pool  = (int8_t)id;
                p.banter_idx   = idx;
                p.banter_lines = doc["banter"]["lines"] | 0;
            }
        }
        s_paramsCb(p);
    } else if (!strcmp(topic, MQTT_TOPIC_CUE)) {
        if (!s_cueCb) return;
        ShowCue c;
        c.session_id = doc["session_id"] | 0UL;
        strlcpy(c.phase, doc["phase"] | "", sizeof(c.phase));
        s_cueCb(c);
    } else {
        ts(); Serial.printf("[mqtt] message on unhandled topic '%s' — ignored\n", topic);
    }
}

static void reconnect() {
    ts(); Serial.print(F("[mqtt] connecting..."));
    bool ok = s_mqtt.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD,
                              MQTT_TOPIC_LEADER_STATUS, 0, true, "{\"status\":\"offline\"}");
    if (ok) {
        Serial.println(F(" connected"));
        s_mqtt.publish(MQTT_TOPIC_LEADER_STATUS, "{\"status\":\"online\"}", true);
        s_mqtt.subscribe(MQTT_TOPIC_PARAMS);
        s_mqtt.subscribe(MQTT_TOPIC_CUE);
    } else {
        Serial.printf(" failed, rc=%d\n", s_mqtt.state());
    }
}

void mqtt_begin() {
    s_mqtt.setServer(MQTT_BROKER_IP, MQTT_BROKER_PORT);
    s_mqtt.setCallback(onMqttMessage);
    // 2026-09-25: default 15s keepalive = broker drops us after ~22s without a
    // ping, and a 20s INTRO blocks the loop longer than that (seen live: drop +
    // LWT "offline" mid-INTRO, cues sent in that window lost). 90s covers the
    // longest clip; cues that arrive during playback wait in the socket.
    s_mqtt.setKeepAlive(90);
    // 2026-09-25 ROOT CAUSE of the bridge pausing cues: PubSubClient's default
    // 256-byte packet limit. The 09-24 banter/aside fields grew the 30s
    // heartbeat to ~400 bytes, so every one was silently dropped; the bridge
    // saw only the connect-time "online" and paused cues 90s later. 1024
    // leaves room for the ~530-byte worst case plus topic/header.
    if (!s_mqtt.setBufferSize(1024)) Serial.println(F("[mqtt] !! setBufferSize(1024) FAILED (no heap?)"));
}

void mqtt_tick() {
    if (!s_mqtt.connected()) {
        uint32_t now = millis();
        if (now - s_lastReconnectAttempt >= MQTT_RECONNECT_INTERVAL_MS) {
            s_lastReconnectAttempt = now;
            reconnect();
        }
        return;
    }
    s_mqtt.loop();
}

bool mqtt_connected() { return s_mqtt.connected(); }

void mqtt_set_params_callback(ShowParamsCallback cb) { s_paramsCb = cb; }
void mqtt_set_cue_callback(ShowCueCallback cb)       { s_cueCb = cb; }

bool mqtt_rx_pending() {
    return s_mqtt.connected() && s_wifiClient.available() > 0;
}

void mqtt_publish_status(const char* json) {
    if (!s_mqtt.connected()) return;
    // 2026-09-25: never again a silent publish failure (see setBufferSize above).
    if (!s_mqtt.publish(MQTT_TOPIC_LEADER_STATUS, json, true)) {
        Serial.printf("[mqtt] !! leader/status publish FAILED (%u bytes, buffer %u) -- bridge will pause cues\n",
                      (unsigned)strlen(json), (unsigned)s_mqtt.getBufferSize());
    }
}
