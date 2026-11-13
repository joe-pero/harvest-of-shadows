#include "espnow_rx.h"
#include "wifi_credentials.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>

#ifndef THIS_NODE
#error "define THIS_NODE (NODE_PATCH_LEFT / NODE_PATCH_RIGHT) in platformio.ini build_flags"
#endif

static EspNowCmdCallback s_callback = nullptr;

// Inbound ring buffer — SPSC (WiFi recv task -> loop()). Matches spooky-tree.
#define ESPNOW_QUEUE_SIZE 8
static Msg              s_queue[ESPNOW_QUEUE_SIZE] = {};
static volatile uint8_t s_qHead = 0;
static volatile uint8_t s_qTail = 0;
static uint16_t         s_qDropped = 0;

static uint32_t s_seq = 0;
static uint32_t s_lastHeartbeatMs = 0;
static const uint32_t HEARTBEAT_INTERVAL_MS = 300000;   // 5 min

static void printTs() {
    uint32_t ms = millis();
    Serial.printf("[%02u:%02u.%03u] ", ms / 60000, (ms % 60000) / 1000, ms % 1000);
}

static void commonRecvCb(const uint8_t* /*mac*/, const uint8_t* data, int len) {
    if (len != (int)sizeof(Msg)) return;
    Msg m;
    memcpy(&m, data, sizeof(Msg));
    if (!msg_valid(m)) return;
    if (m.target != (uint8_t)NODE_BROADCAST && m.target != (uint8_t)THIS_NODE) return;

    uint8_t next = (uint8_t)((s_qTail + 1) % ESPNOW_QUEUE_SIZE);
    if (next == s_qHead) { s_qDropped++; return; }
    s_queue[s_qTail] = m;
    s_qTail = next;
}

#if ESP_IDF_VERSION_MAJOR >= 5
static void onRecvV5(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    commonRecvCb(info->src_addr, data, len);
}
#else
static void onRecvLegacy(const uint8_t* mac, const uint8_t* data, int len) {
    commonRecvCb(mac, data, len);
}
#endif

static void onSendStatus(const uint8_t*, esp_now_send_status_t) {}

static Msg makeMsg(Command cmd) {
    Msg m = {};
    m.command = (uint8_t)cmd;
    m.sender  = (uint8_t)THIS_NODE;
    m.target  = (uint8_t)NODE_BROADCAST;
    m.seq     = s_seq++;
    msg_seal(m);
    return m;
}

void espnowInit() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    // 2026-09-27: joins the AP too, for Home Assistant (ha_mqtt.cpp). All APs are on
    // ch 11 (NETWORK_CONSTRAINTS C1), so ESP-NOW stays on the mesh channel. No AP ->
    // lock ch 11 and run ESP-NOW-only; ha_mqtt.cpp keeps retrying the AP.
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    printTs(); Serial.printf("[espnow] WiFi assoc to \"%s\"", WIFI_SSID);
    uint32_t deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) { delay(250); Serial.print('.'); }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        printTs(); Serial.printf("[espnow] WiFi up: IP %s RSSI %d channel %d%s\n",
                                 WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel(),
                                 WiFi.channel() == WIFI_CHANNEL ? "" : "  !! NOT the mesh channel");
    } else {
        printTs(); Serial.println(F("[espnow] WiFi FAILED at boot — ESP-NOW only for now, HA retries"));
        esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
        printTs(); Serial.printf("[espnow] channel locked to %d\n", WIFI_CHANNEL);
    }

    if (esp_now_init() != ESP_OK) { printTs(); Serial.println("[espnow] init failed"); return; }
    esp_now_register_send_cb(onSendStatus);
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_now_register_recv_cb(onRecvV5);
#else
    esp_now_register_recv_cb(onRecvLegacy);
#endif

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, MAC_BROADCAST, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) { printTs(); Serial.println("[espnow] add broadcast peer failed"); }

    printTs(); Serial.printf("[espnow] ready — node 0x%02X (%s)\n",
                             (unsigned)THIS_NODE, WiFi.macAddress().c_str());
}

void espnowSetCallback(EspNowCmdCallback cb) { s_callback = cb; }

void espnowTick() {
    while (s_qHead != s_qTail) {
        Msg m = s_queue[s_qHead];
        s_qHead = (uint8_t)((s_qHead + 1) % ESPNOW_QUEUE_SIZE);
        if (s_callback) s_callback(m);
    }
    if (s_qDropped > 0) {
        printTs(); Serial.printf("[espnow] %u frame(s) dropped — queue full\n", s_qDropped);
        s_qDropped = 0;
    }
}

void espnowSendNodeOnline() {
    Msg m = makeMsg(CMD_NODE_ONLINE);
    esp_now_send(MAC_BROADCAST, (const uint8_t*)&m, sizeof(Msg));
}

void espnowHeartbeatTick() {
    uint32_t now = millis();
    if (now - s_lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
        s_lastHeartbeatMs = now;
        espnowSendNodeOnline();
    }
}
