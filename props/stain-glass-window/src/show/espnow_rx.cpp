#include "espnow_rx.h"
#include "wifi_credentials.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>

static EspNowRxCallback s_cb = nullptr;

// SPSC ring: WiFi task writes, loop() reads. 32 slots = several 9-frame Scarecrow bursts.
#define Q_SIZE 32
static Msg s_q[Q_SIZE];
static volatile uint8_t s_head = 0, s_tail = 0;
static volatile uint32_t s_rxAll = 0, s_rxBadLen = 0, s_rxBadSeal = 0, s_rxNotUs = 0, s_rxQueued = 0;
static volatile uint32_t s_qDropped = 0, s_lastScarecrowMs = 0;
static uint32_t s_qDroppedReported = 0, s_chRelocks = 0, s_lastInitTry = 0, s_lastChCheck = 0;
static bool s_ready = false;

static void tstamp() { Serial.printf("[%7lus] ", (unsigned long)(millis() / 1000)); }

static void commonRecv(const uint8_t* mac, const uint8_t* data, int len) {
    (void)mac;
    s_rxAll++;
    if (len != (int)sizeof(Msg)) { s_rxBadLen++; return; }
    Msg m; memcpy(&m, data, sizeof(Msg));
    if (!msg_valid(m)) { s_rxBadSeal++; return; }
    if (m.sender == (uint8_t)NODE_SCARECROW_A1S) s_lastScarecrowMs = millis();
    if (m.target != (uint8_t)NODE_BROADCAST && m.target != (uint8_t)NODE_STAIN_GLASS) { s_rxNotUs++; return; }
    uint8_t next = (uint8_t)((s_tail + 1) % Q_SIZE);
    if (next == s_head) { s_qDropped++; return; }
    s_q[s_tail] = m; s_tail = next; s_rxQueued++;
}

#if ESP_IDF_VERSION_MAJOR >= 5
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* d, int n) { commonRecv(info->src_addr, d, n); }
#else
static void onRecv(const uint8_t* mac, const uint8_t* d, int n) { commonRecv(mac, d, n); }
#endif

static void sendNodeOnline() {
    Msg m = {};
    m.command = (uint8_t)CMD_NODE_ONLINE; m.sender = (uint8_t)NODE_STAIN_GLASS; m.target = (uint8_t)NODE_BROADCAST;
    m.session_id = esp_random();
    msg_seal(m);
    esp_err_t e = esp_now_send(MAC_BROADCAST, reinterpret_cast<const uint8_t*>(&m), sizeof(Msg));
    tstamp(); Serial.printf("[espnow] NODE_ONLINE %s\n", e == ESP_OK ? "sent (Scarecrow answers with a theme resync)" : "NOT queued");
}

static bool start() {
    s_lastInitTry = millis();
    if (esp_now_init() != ESP_OK) { tstamp(); Serial.println(F("[espnow] ALARM: init FAILED - deaf to the show; retry in 30 s")); return false; }
    esp_now_register_recv_cb(onRecv);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, MAC_BROADCAST, 6);
    if (!esp_now_is_peer_exist(MAC_BROADCAST) && esp_now_add_peer(&peer) != ESP_OK) {
        tstamp(); Serial.println(F("[espnow] ALARM: broadcast peer FAILED - retry in 30 s"));
        esp_now_deinit(); return false;
    }
    s_ready = true;
    tstamp(); Serial.printf("[espnow] ready on channel %d, listening for the Scarecrow\n", WIFI_CHANNEL);
    sendNodeOnline();
    return true;
}

void espnowRxInit(EspNowRxCallback cb) {
    s_cb = cb;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    start();
}

void espnowRxTick() {
    while (s_head != s_tail) {
        Msg m = s_q[s_head];
        s_head = (uint8_t)((s_head + 1) % Q_SIZE);
        if (s_cb) s_cb(m);
    }
    if (s_qDropped != s_qDroppedReported) {
        tstamp(); Serial.printf("[espnow] WARN: %lu frame(s) dropped, queue full\n", (unsigned long)(s_qDropped - s_qDroppedReported));
        s_qDroppedReported = s_qDropped;
    }
    uint32_t now = millis();
    if (!s_ready && now - s_lastInitTry >= 30000) start();
    if (now - s_lastChCheck >= 10000) {              // never joins an AP, but a wrong channel = silent deafness
        s_lastChCheck = now;
        uint8_t ch = 0; wifi_second_chan_t sc;
        if (esp_wifi_get_channel(&ch, &sc) == ESP_OK && ch != WIFI_CHANNEL) {
            esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
            s_chRelocks++;
            tstamp(); Serial.printf("[espnow] WARN: channel drift %u -> re-locked to %d\n", (unsigned)ch, WIFI_CHANNEL);
        }
    }
}

EspNowRxStats espnowRxStats() {
    EspNowRxStats s;
    s.ready = s_ready; s.rxAll = s_rxAll; s.rxBadLen = s_rxBadLen; s.rxBadSeal = s_rxBadSeal; s.rxNotUs = s_rxNotUs;
    s.rxQueued = s_rxQueued; s.qDropped = s_qDropped; s.chRelocks = s_chRelocks; s.lastScarecrowMs = s_lastScarecrowMs;
    uint8_t ch = 0; wifi_second_chan_t sc;
    s.channel = esp_wifi_get_channel(&ch, &sc) == ESP_OK ? ch : 0;
    return s;
}
