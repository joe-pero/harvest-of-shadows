#include "ha_proj.h"
#include "wifi_credentials.h"
#include <WiFi.h>
#include <PubSubClient.h>

// HA's broker (detect-server), same as the Singing Pumpkins: wifi_credentials.h MQTT_BROKER_IP is the
// Workshop show broker (.237), which is off whenever the Workshop PC is.
static const char* HA_BROKER_IP = "192.168.1.x";
static const char* CLIENT_ID = "stain_glass";
static const char* T_STATUS  = "harvest/stainglass/status";
static const char* TAG[2]    = {"wp", "dp"};
static const char* NAME[2]   = {"Window projector", "Door projector"};

static WiFiClient   s_net;
static PubSubClient s_mqtt(s_net);
static bool     s_wifiUp = false;
static uint32_t s_cmds = 0, s_reconnects = 0;

static void ts() { Serial.printf("[%7lus] ", (unsigned long)(millis() / 1000)); }
static String topic(int i, const char* leaf) { return String("harvest/stainglass/") + TAG[i] + "/" + leaf; }

static void onMessage(char* t, byte* payload, unsigned int len) {
    String msg;
    for (unsigned int k = 0; k < len; k++) msg += (char)payload[k];
    msg.trim();
    for (int i = 0; i < 2; i++) {
        if (topic(i, "set") != t) continue;
        s_cmds++;
        ts(); Serial.printf("[ha] %s <- %s\n", t, msg.c_str());
        if (msg.equalsIgnoreCase("ON")) sgProjSet(i, true, "HA");
        else if (msg.equalsIgnoreCase("OFF")) sgProjSet(i, false, "HA");
        else { ts(); Serial.printf("[ha] !! %s: '%s' is not ON/OFF, ignored\n", TAG[i], msg.c_str()); }
        return;
    }
}

static void disc(const char* component, const String& objectId, const String& body) {
    String t = String("homeassistant/") + component + "/stain_glass/" + objectId + "/config";
    String payload = "{" + body + ",\"unique_id\":\"stain_glass_" + objectId + "\","
        "\"device\":{\"identifiers\":[\"stain_glass\"],\"name\":\"Stain Glass + Front Door\","
        "\"manufacturer\":\"Harvest of Shadows\",\"model\":\"ESP32 + 2x Hitachi RS-232\"},"
        "\"availability_topic\":\"harvest/stainglass/status\"}";
    if (!s_mqtt.publish(t.c_str(), payload.c_str(), true)) {
        ts(); Serial.printf("[ha] !! discovery publish failed: %s (%u bytes)\n", objectId.c_str(), (unsigned)payload.length());
    }
}

static void publishDiscovery() {
    for (int i = 0; i < 2; i++) {
        String id = String(TAG[i]) + "_projector";
        disc("switch", id, String("\"name\":\"") + NAME[i] + "\",\"icon\":\"mdi:projector\","
             "\"command_topic\":\"" + topic(i, "set") + "\",\"state_topic\":\"" + topic(i, "want") + "\"");
        disc("sensor", id + "_power", String("\"name\":\"") + NAME[i] + " power\",\"icon\":\"mdi:projector\","
             "\"state_topic\":\"" + topic(i, "power") + "\"");
    }
}

static void publishState(bool force) {
    static int s_want[2] = {-1, -1};
    static String s_st[2];
    for (int i = 0; i < 2; i++) {
        int want = sgProjWant(i) ? 1 : 0;
        if (force || want != s_want[i]) {
            if (s_mqtt.publish(topic(i, "want").c_str(), want ? "ON" : "OFF", true)) s_want[i] = want;
        }
        String st = sgProjState(i);
        if (force || st != s_st[i]) {
            if (s_mqtt.publish(topic(i, "power").c_str(), st.c_str(), true)) s_st[i] = st;
        }
    }
}

static void wifiTick() {
    static uint32_t lastCheck = 0, lastRetry = 0, retries = 0;
    if (millis() - lastCheck < 3000) return;
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
        if (s_wifiUp) { s_wifiUp = false; ts(); Serial.println(F("[ha] WiFi dropped - retrying every 10 s (ESP-NOW unaffected)")); }
        if (millis() - lastRetry >= 10000) {
            lastRetry = millis();
            retries++;
            if (retries == 1 || retries % 30 == 0) { ts(); Serial.printf("[ha] WiFi retry #%lu\n", (unsigned long)retries); }
            WiFi.disconnect(false, false);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);   // channel-locked: never scans off the mesh channel
        }
        return;
    }
    if (!s_wifiUp) {
        s_wifiUp = true;
        ts(); Serial.printf("[ha] WiFi up after %lu retries: IP %s RSSI %d channel %d\n", (unsigned long)retries,
                            WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
        retries = 0;
    }
}

static void mqttReconnect() {
    if (WiFi.status() != WL_CONNECTED) return;
    static uint32_t lastAttempt = 0;
    if (millis() - lastAttempt < 5000) return;
    lastAttempt = millis();
    if (s_mqtt.connect(CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD, T_STATUS, 1, true, "offline")) {
        s_reconnects++;
        ts(); Serial.printf("[ha] MQTT connected to HA broker (#%lu)\n", (unsigned long)s_reconnects);
        s_mqtt.publish(T_STATUS, "online", true);
        for (int i = 0; i < 2; i++) s_mqtt.subscribe(topic(i, "set").c_str());
        publishDiscovery();
        publishState(true);
    } else {
        ts(); Serial.printf("[ha] MQTT connect failed, rc=%d\n", s_mqtt.state());
    }
}

void haProjBegin() {
    s_mqtt.setServer(HA_BROKER_IP, MQTT_BROKER_PORT);
    s_mqtt.setCallback(onMessage);
    s_mqtt.setBufferSize(1024);
    // Same stall caps as eave_coordinator / Singing Pumpkins (2026-09-16 overnight-outage fix)
    s_mqtt.setSocketTimeout(2);
    s_net.setTimeout(1);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
    ts(); Serial.printf("[ha] HA broker %s, WiFi \"%s\" (ch %d)\n", HA_BROKER_IP, WIFI_SSID, WIFI_CHANNEL);
}

void haProjTick() {
    wifiTick();
    if (!s_mqtt.connected()) { mqttReconnect(); return; }
    s_mqtt.loop();
    static uint32_t lastState = 0;
    if (millis() - lastState >= 500) { lastState = millis(); publishState(false); }
}

void haProjMon() {
    ts(); Serial.printf("[ha] WiFi %s, MQTT %s, %lu HA commands, %lu connects\n",
                        WiFi.status() == WL_CONNECTED ? "up" : "DOWN", s_mqtt.connected() ? "up" : "DOWN",
                        (unsigned long)s_cmds, (unsigned long)s_reconnects);
}
