#include "espnow_rx.h"
#include "BlackBox.h"
#include "wifi_credentials.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>

#ifndef THIS_NODE
#error "define THIS_NODE (NODE_PATCH_LEFT / NODE_PATCH_RIGHT) in platformio.ini build_flags"
#endif

static EspNowCmdCallback s_callback = nullptr;

// Inbound ring buffer — SPSC (WiFi recv task -> loop()). 32 slots: a Scarecrow
// theme burst is 9 frames in ~0.5 s and loop() can block 1-3 s in an MP3 start.
#define ESPNOW_QUEUE_SIZE 32
static Msg              s_queue[ESPNOW_QUEUE_SIZE] = {};
static volatile uint8_t s_qHead = 0;
static volatile uint8_t s_qTail = 0;
static uint16_t         s_qDropped = 0;

static uint32_t s_sessionId = 0;
static uint16_t s_seq       = 0;

// Counters (WiFi task writes, loop reads; a torn read only skews one status line)
static volatile uint32_t s_rxAll = 0, s_rxBadLen = 0, s_rxBadSeal = 0, s_rxNotUs = 0, s_rxQueued = 0;
static volatile uint32_t s_qDroppedTotal = 0, s_lastScarecrowMs = 0;
static volatile uint8_t  s_badMac[6] = {};
static volatile int      s_badLen = 0;
static volatile uint32_t s_txOk = 0, s_txFail = 0;
static uint32_t s_onlineSentMs = 0, s_lastInitTry = 0, s_chRelocks = 0;
static bool     s_ready = false;

static void printTs() { logTs(); }

static void commonRecvCb(const uint8_t* mac, const uint8_t* data, int len) {
    s_rxAll++;
    if (len != (int)sizeof(Msg)) {
        s_rxBadLen++; s_badLen = len;
        if (mac) for (int i = 0; i < 6; i++) s_badMac[i] = mac[i];
        return;
    }
    Msg m;
    memcpy(&m, data, sizeof(Msg));
    if (!msg_valid(m)) {
        s_rxBadSeal++; s_badLen = len;
        if (mac) for (int i = 0; i < 6; i++) s_badMac[i] = mac[i];
        return;
    }
    if (m.sender == (uint8_t)NODE_SCARECROW_A1S) s_lastScarecrowMs = millis();
    if (m.target != (uint8_t)NODE_BROADCAST && m.target != (uint8_t)THIS_NODE) { s_rxNotUs++; return; }

    uint8_t next = (uint8_t)((s_qTail + 1) % ESPNOW_QUEUE_SIZE);
    if (next == s_qHead) { s_qDropped++; s_qDroppedTotal++; return; }
    s_queue[s_qTail] = m;
    s_qTail = next;
    s_rxQueued++;
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

static void onSendStatus(const uint8_t* /*mac*/, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) s_txOk++; else s_txFail++;
}

static bool espnowStart() {
    s_lastInitTry = millis();
    if (esp_now_init() != ESP_OK) {
        printTs(); Log.println("[espnow] !! Init FAILED — no fleet radio; retrying every 30 s");
        return false;
    }
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
    if (!esp_now_is_peer_exist(MAC_BROADCAST) && esp_now_add_peer(&peer) != ESP_OK) {
        printTs(); Log.println("[espnow] !! Failed to add broadcast peer — retrying every 30 s");
        esp_now_deinit();
        return false;
    }
    s_ready = true;
    printTs(); Log.printf("[espnow] Ready; session=0x%08X\n", (unsigned)s_sessionId);
    return true;
}

void espnowInit() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);            // no modem-sleep — keep ESP-NOW RX latency low
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    printTs(); Log.printf("[espnow] Channel locked to %d\n", WIFI_CHANNEL);
    s_sessionId = esp_random();
    espnowStart();
}

void espnowSetCallback(EspNowCmdCallback cb) { s_callback = cb; }

void espnowTick() {
    while (s_qHead != s_qTail) {
        Msg m = s_queue[s_qHead];
        s_qHead = (uint8_t)((s_qHead + 1) % ESPNOW_QUEUE_SIZE);
        if (s_callback) s_callback(m);
    }
    if (s_qDropped > 0) {
        printTs(); Log.printf("[espnow] WARN %u frame(s) dropped — queue was full\n", s_qDropped);
        s_qDropped = 0;
    }
}

// The node never joins an AP, so nothing should move it off the fleet channel —
// but a wrong channel = every frame lost with no error. Check before each send.
static void ensureChannel() {
    uint8_t ch = 0; wifi_second_chan_t sc;
    if (esp_wifi_get_channel(&ch, &sc) == ESP_OK && ch == WIFI_CHANNEL) return;
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    s_chRelocks++;
    printTs(); Log.printf("[espnow] !! channel drift: was %u, re-locked to %d (x%lu)\n",
                         (unsigned)ch, WIFI_CHANNEL, (unsigned long)s_chRelocks);
}

void espnowSendNodeOnline() {
    if (!s_ready) { printTs(); Log.println("[espnow] NODE_ONLINE not sent — radio not up"); return; }
    ensureChannel();
    Msg m = {};
    m.command = (uint8_t)CMD_NODE_ONLINE; m.sender = (uint8_t)THIS_NODE; m.target = (uint8_t)NODE_BROADCAST;
    m.session_id = s_sessionId; m.seq = s_seq++;
    msg_seal(m);
    esp_now_send(MAC_BROADCAST, reinterpret_cast<const uint8_t*>(&m), sizeof(Msg));
    s_onlineSentMs = millis(); if (!s_onlineSentMs) s_onlineSentMs = 1;
    printTs(); Log.println("[espnow] Sent NODE_ONLINE");
}

uint32_t espnowLastOnlineMs() { return s_onlineSentMs; }

void espnowHeartbeatTick() {
    if (!s_ready && (millis() - s_lastInitTry) >= 30000) {
        if (espnowStart()) espnowSendNodeOnline();
    }
}

EspNowStats espnowStats() {
    EspNowStats st;
    st.ready = s_ready; st.txOk = s_txOk; st.txFail = s_txFail; st.chRelocks = s_chRelocks;
    st.rxAll = s_rxAll; st.rxBadLen = s_rxBadLen; st.rxBadSeal = s_rxBadSeal; st.rxNotUs = s_rxNotUs;
    st.rxQueued = s_rxQueued; st.qDropped = s_qDroppedTotal; st.lastScarecrowMs = s_lastScarecrowMs;
    for (int i = 0; i < 6; i++) st.lastBadMac[i] = s_badMac[i];
    st.lastBadLen = s_badLen;
    uint8_t ch = 0; wifi_second_chan_t sc;
    st.channel = esp_wifi_get_channel(&ch, &sc) == ESP_OK ? ch : 0;
    return st;
}
