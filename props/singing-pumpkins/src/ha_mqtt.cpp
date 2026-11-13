#include "ha_mqtt.h"
#include "brightsign.h"
#include "projector_ir.h"
#include "skeletons.h"
#include "wifi_credentials.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <PubSubClient.h>

// HA's broker (detect-server). The shared wifi_credentials.h MQTT_BROKER_IP is the
// Workshop show broker (.237); the pumpkins talk to HA's broker instead.
static const char* HA_BROKER_IP = "192.168.1.x";

static const char* CLIENT_ID   = "singing_pumpkins";
static const char* T_STATUS    = "harvest/pumpkins/status";     // online / offline (LWT)
static const char* T_CMD       = "harvest/pumpkins/cmd";        // raw card line
static const char* T_PLAY      = "harvest/pumpkins/play";       // select: "<n> - <group> - <title>"
static const char* T_VOL_SET   = "harvest/pumpkins/volume/set";
static const char* T_VOL       = "harvest/pumpkins/volume";
static const char* T_NOW       = "harvest/pumpkins/now";
static const char* T_NOW_PICK  = "harvest/pumpkins/now_pick";   // select state: label of what's playing
static const char* T_MODE      = "harvest/pumpkins/mode";
static const char* T_LINK      = "harvest/pumpkins/link";
static const char* T_ERR       = "harvest/pumpkins/error";
static const char* T_SKEL_SET  = "harvest/pumpkins/skel/set";   // switch: ON / OFF
static const char* T_SKEL      = "harvest/pumpkins/skel";
static const char* T_SKEL_LAST = "harvest/pumpkins/skel/last";  // last moment / decision
static const char* T_IR_LAST   = "harvest/pumpkins/projector_ir";  // last IR action (HA gates the cord cut on it)

static WiFiClient   s_net;
static PubSubClient s_mqtt(s_net);
static bool     s_wifiUp = false;
static uint32_t s_publishedSongsVersion = 0;

static void ts() { Serial.printf("[%lu] ", (unsigned long)millis()); }

// ---------------------------------------------------------------- commands ---
static void onMessage(char* topic, byte* payload, unsigned int len) {
    String msg;
    msg.reserve(len);
    for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
    msg.trim();
    ts(); Serial.printf("[ha] %s <- '%s'\n", topic, msg.c_str());

    if (!strcmp(topic, T_PLAY)) {
        int n = msg.toInt();                          // label starts with its number
        if (n > 0) brightsign_jukebox((uint16_t)n, true);
        else { ts(); Serial.println(F("[ha] !! play: no number in the pick, ignored")); }
    } else if (!strcmp(topic, T_VOL_SET)) {
        int v = constrain(msg.toInt(), 0, 100);
        char b[12]; snprintf(b, sizeof b, "VOL %d", v);
        brightsign_send(b);
        s_mqtt.publish(T_VOL, String(v).c_str(), true);
    } else if (!strcmp(topic, T_SKEL_SET)) {
        skel_set_enabled(msg.equalsIgnoreCase("ON"));
    } else if (!strcmp(topic, T_CMD)) {
        if (msg.equalsIgnoreCase("IRPWR")) projector_ir_power();   // ESP32-local, not sent to the card
        else if (skel_console(msg.c_str())) ;                      // SKEL ... = ESP32-local
        else if (msg.length() && msg.length() < 40) brightsign_send(msg.c_str());
    }
}

// --------------------------------------------------------------- discovery ---
static const char* DEVICE =
    "\"device\":{\"identifiers\":[\"singing_pumpkins\"],\"name\":\"Singing Pumpkins\","
    "\"manufacturer\":\"Harvest of Shadows\",\"model\":\"ESP32 + BrightSign XT1143\"},"
    "\"availability_topic\":\"harvest/pumpkins/status\"";

static void disc(const char* component, const char* objectId, const String& body) {
    String topic = String("homeassistant/") + component + "/singing_pumpkins/" + objectId + "/config";
    String payload = "{" + body + ",\"unique_id\":\"singing_pumpkins_" + objectId + "\"," + DEVICE + "}";
    if (!s_mqtt.publish(topic.c_str(), payload.c_str(), true)) {
        ts(); Serial.printf("[ha] !! discovery publish failed: %s (%u bytes)\n", objectId, (unsigned)payload.length());
    }
}

static void publishStaticDiscovery() {
    disc("button", "start_show", "\"name\":\"Start show\",\"icon\":\"mdi:play-circle\","
         "\"command_topic\":\"harvest/pumpkins/cmd\",\"payload_press\":\"SHOW FORCE\"");
    disc("button", "end_show", "\"name\":\"End show\",\"icon\":\"mdi:stop-circle\","
         "\"command_topic\":\"harvest/pumpkins/cmd\",\"payload_press\":\"END\"");
    disc("button", "skip", "\"name\":\"Skip\",\"icon\":\"mdi:skip-next\","
         "\"command_topic\":\"harvest/pumpkins/cmd\",\"payload_press\":\"SKIP\"");
    disc("button", "projector_power", "\"name\":\"Projector power\",\"icon\":\"mdi:projector\","
         "\"command_topic\":\"harvest/pumpkins/cmd\",\"payload_press\":\"IRPWR\"");
    disc("switch", "skeletons", "\"name\":\"Skeletons\",\"icon\":\"mdi:skull\","
         "\"command_topic\":\"harvest/pumpkins/skel/set\",\"state_topic\":\"harvest/pumpkins/skel\"");
    disc("button", "skel_test", "\"name\":\"Skeleton jaw test\",\"icon\":\"mdi:skull-outline\","
         "\"command_topic\":\"harvest/pumpkins/cmd\",\"payload_press\":\"SKEL B 1000\"");
    disc("button", "skel_clack", "\"name\":\"Skeleton clack test\",\"icon\":\"mdi:skull-crossbones\","
         "\"command_topic\":\"harvest/pumpkins/cmd\",\"payload_press\":\"SKEL CLACK 4\"");
    disc("sensor", "projector_ir", "\"name\":\"Projector IR last\",\"icon\":\"mdi:remote\","
         "\"state_topic\":\"harvest/pumpkins/projector_ir\"");
    disc("sensor", "skel_last", "\"name\":\"Last skeleton moment\",\"icon\":\"mdi:skull\","
         "\"state_topic\":\"harvest/pumpkins/skel/last\"");
    disc("number", "volume", "\"name\":\"Volume\",\"icon\":\"mdi:volume-high\",\"min\":0,\"max\":100,\"step\":5,"
         "\"command_topic\":\"harvest/pumpkins/volume/set\",\"state_topic\":\"harvest/pumpkins/volume\"");
    disc("sensor", "now", "\"name\":\"Now playing\",\"icon\":\"mdi:music\",\"state_topic\":\"harvest/pumpkins/now\"");
    disc("sensor", "mode", "\"name\":\"Mode\",\"icon\":\"mdi:pumpkin\",\"state_topic\":\"harvest/pumpkins/mode\"");
    disc("sensor", "error", "\"name\":\"Last card error\",\"icon\":\"mdi:alert\",\"state_topic\":\"harvest/pumpkins/error\"");
    disc("binary_sensor", "link", "\"name\":\"Player link\",\"device_class\":\"connectivity\","
         "\"state_topic\":\"harvest/pumpkins/link\"");
}

// The pick list comes from the card, so HA always matches what's on the SD card.
static void publishSelectDiscovery() {
    const auto& songs = brightsign_songs();
    if (songs.empty()) return;
    String body = "\"name\":\"Play now\",\"icon\":\"mdi:playlist-music\","
                  "\"command_topic\":\"harvest/pumpkins/play\",\"state_topic\":\"harvest/pumpkins/now_pick\","
                  "\"options\":[";
    for (size_t i = 0; i < songs.size(); i++) {
        if (i) body += ',';
        body += '"';
        for (char c : songs[i].label) { if (c == '"' || c == '\\') body += '\\'; body += c; }
        body += '"';
    }
    body += "]";
    disc("select", "play", body);
    s_publishedSongsVersion = brightsign_songs_version();
    ts(); Serial.printf("[ha] pick list published to HA: %u entries\n", (unsigned)songs.size());
}

// ------------------------------------------------------------ state updates ---
static void publishStateIfChanged(bool force) {
    static String s_now, s_mode, s_err;
    static int s_link = -1;
    const String& now = brightsign_now_line();
    if (force || now != s_now) {
        s_now = now;
        // "12 blood_in_the_corn SUNO suno 254.9" -> show the pick label when we have it
        String shown = now;
        int n = brightsign_now_n();
        for (const auto& s : brightsign_songs()) if (s.n == n) { shown = s.label; break; }
        s_mqtt.publish(T_NOW, shown.length() ? shown.c_str() : "none", true);
        if (shown != now) s_mqtt.publish(T_NOW_PICK, shown.c_str(), true);
    }
    String mode = brightsign_card_mode();
    if (force || mode != s_mode) { s_mode = mode; s_mqtt.publish(T_MODE, mode.c_str(), true); }
    const String& err = brightsign_last_error();
    if (force || err != s_err) { s_err = err; s_mqtt.publish(T_ERR, err.length() ? err.c_str() : "none", true); }
    static uint32_t s_skelVer = 0;
    if (force || skel_version() != s_skelVer) {
        s_skelVer = skel_version();
        s_mqtt.publish(T_SKEL, skel_enabled() ? "ON" : "OFF", true);
        s_mqtt.publish(T_SKEL_LAST, skel_last().c_str(), true);
    }
    static uint32_t s_irVer = 0xFFFFFFFF;
    if (force || projector_ir_version() != s_irVer) {
        if (s_mqtt.publish(T_IR_LAST, projector_ir_last(), true)) s_irVer = projector_ir_version();
    }
    int link = brightsign_link_ok() ? 1 : 0;
    if (force || link != s_link) { s_link = link; s_mqtt.publish(T_LINK, link ? "ON" : "OFF", true); }
}

// -------------------------------------------------------------------- WiFi ---
static void wifiTick() {
    static uint32_t lastCheck = 0, lastRetry = 0, retries = 0;
    if (millis() - lastCheck < 3000) return;
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
        if (s_wifiUp) { s_wifiUp = false; ts(); Serial.println(F("[ha] WiFi dropped — retrying every 10 s (ESP-NOW unaffected)")); }
        if (millis() - lastRetry >= 10000) {
            lastRetry = millis();
            retries++;
            ts(); Serial.printf("[ha] WiFi retry #%lu\n", (unsigned long)retries);
            WiFi.disconnect(false, false);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
        return;
    }
    if (!s_wifiUp) {
        s_wifiUp = true;
        ts(); Serial.printf("[ha] WiFi up after %lu retries: IP %s RSSI %d channel %d\n", (unsigned long)retries,
                            WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
        retries = 0;
    }
    if (WiFi.channel() != WIFI_CHANNEL) {
        ts(); Serial.printf("[ha] !! CHANNEL DRIFT: AP on %d, mesh on %d — ESP-NOW cues will be missed\n",
                            WiFi.channel(), WIFI_CHANNEL);
    }
}

// -------------------------------------------------------------------- MQTT ---
static void mqttReconnect() {
    if (WiFi.status() != WL_CONNECTED) return;
    static uint32_t lastAttempt = 0;
    if (millis() - lastAttempt < 5000) return;
    lastAttempt = millis();
    ts(); Serial.print(F("[ha] MQTT connecting..."));
    if (s_mqtt.connect(CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD, T_STATUS, 1, true, "offline")) {
        Serial.println(F(" connected"));
        s_mqtt.publish(T_STATUS, "online", true);
        s_mqtt.subscribe(T_CMD);
        s_mqtt.subscribe(T_PLAY);
        s_mqtt.subscribe(T_VOL_SET);
        s_mqtt.subscribe(T_SKEL_SET);
        publishStaticDiscovery();
        publishSelectDiscovery();
        publishStateIfChanged(true);
    } else {
        Serial.printf(" failed, rc=%d\n", s_mqtt.state());
    }
}

void ha_begin() {
    s_mqtt.setServer(HA_BROKER_IP, MQTT_BROKER_PORT);
    s_mqtt.setCallback(onMessage);
    s_mqtt.setBufferSize(12288);      // the select's option list is ~7 KB
    // Same stall caps as eave_coordinator (2026-09-16 overnight-outage fix):
    s_mqtt.setSocketTimeout(2);
    s_net.setTimeout(1);
    ts(); Serial.printf("[ha] HA broker %s, WiFi \"%s\"\n", HA_BROKER_IP, WIFI_SSID);
}

void ha_tick() {
    wifiTick();
    if (!s_mqtt.connected()) { mqttReconnect(); return; }
    s_mqtt.loop();
    if (brightsign_songs_version() != s_publishedSongsVersion) publishSelectDiscovery();
    static uint32_t lastState = 0;
    if (millis() - lastState >= 500) { lastState = millis(); publishStateIfChanged(false); }
}

bool ha_connected() { return s_mqtt.connected(); }
