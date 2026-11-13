#include "espnow_handler.h"
#include "BlackBox.h"
#include "wifi_credentials.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>

static EspNowCmdCallback s_callback        = nullptr;

// Inbound ring buffer — single producer (WiFi/ESP-NOW recv task, commonRecvCb),
// single consumer (espnowTick() from loop()). Direct port of Spooky Tree's
// espnow_handler.cpp — see its IMPROVEMENTS.md 1.1 for why the whole queue is
// drained in one pass rather than one message per call.
// 32 (was 8, 2026-09-26): one Scarecrow theme burst is 9 frames in ~0.5 s and
// loop() can block 1-3 s in an MP3 start/poll; plus other nodes' broadcasts.
#define ESPNOW_QUEUE_SIZE 32
static Msg              s_queue[ESPNOW_QUEUE_SIZE] = {};
static volatile uint8_t s_qHead                    = 0;  // next slot to read
static volatile uint8_t s_qTail                    = 0;  // next slot to write
static uint16_t         s_qDropped                 = 0;  // queue-full drop count (diagnostic)

static uint32_t          s_sessionId       = 0;
static uint16_t          s_seq             = 0;
static uint32_t          s_onlineSentMs    = 0;     // last NODE_ONLINE (0 = never)

static void printTs() { logTs(); }

// Receive counters (2026-10-01 black box, same as the Spooky Tree). Written on
// the WiFi task, read by the loop's status line; a torn read only skews one line.
static volatile uint32_t s_rxAll = 0, s_rxBadLen = 0, s_rxBadSeal = 0, s_rxNotUs = 0, s_rxQueued = 0;
static volatile uint32_t s_qDroppedTotal = 0, s_lastScarecrowMs = 0;
static volatile uint8_t  s_badMac[6] = {};
static volatile int      s_badLen = 0;
static volatile uint32_t s_txOk = 0, s_txFail = 0;

// ── IDF 4/5 receive callback bridge ──────────────────────────────────────────

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
    // Broadcast-only architecture: filter by msg.target so each node only processes
    // messages addressed to it or to all nodes.
    if (m.target != (uint8_t)NODE_BROADCAST && m.target != (uint8_t)NODE_PUMPKIN_MEN) { s_rxNotUs++; return; }

    uint8_t next = (uint8_t)((s_qTail + 1) % ESPNOW_QUEUE_SIZE);
    if (next == s_qHead) {
        s_qDropped++; s_qDroppedTotal++;
        return;
    }
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

// ── Outbound message factory ──────────────────────────────────────────────────

static Msg makeMsg(Command cmd) {
    Msg m = {};
    m.command    = (uint8_t)cmd;
    m.sender     = (uint8_t)NODE_PUMPKIN_MEN;
    m.target     = (uint8_t)NODE_BROADCAST;
    m.flags      = 0;
    m.session_id = s_sessionId;
    m.seq        = s_seq++;
    msg_seal(m);
    return m;
}

// ── Public API ────────────────────────────────────────────────────────────────

void espnowInit() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);           // no modem-sleep — keep ESP-NOW RX latency low
    esp_wifi_set_ps(WIFI_PS_NONE);

    // No AP connection — ESP-NOW only. Lock radio to configured channel.
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    printTs(); Log.printf("[espnow] Channel locked to %d\n", WIFI_CHANNEL);

    if (esp_now_init() != ESP_OK) {
        printTs(); Log.println("[espnow] Init failed");
        return;
    }

    esp_now_register_send_cb(onSendStatus);

#if ESP_IDF_VERSION_MAJOR >= 5
    esp_now_register_recv_cb(onRecvV5);
#else
    esp_now_register_recv_cb(onRecvLegacy);
#endif

    // Broadcast-only architecture — fire-and-forget, show continues if any
    // node is offline. Filter by msg.target in commonRecvCb.
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, MAC_BROADCAST, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK)
        { printTs(); Log.println("[espnow] Failed to add broadcast peer"); }

    s_sessionId = esp_random();
    printTs(); Log.printf("[espnow] Ready; session=0x%08X\n", (unsigned)s_sessionId);
}

void espnowSetCallback(EspNowCmdCallback cb) {
    s_callback = cb;
}

void espnowTick() {
    while (s_qHead != s_qTail) {
        Msg m = s_queue[s_qHead];
        s_qHead = (uint8_t)((s_qHead + 1) % ESPNOW_QUEUE_SIZE);
        if (s_callback) s_callback(m);
    }
    if (s_qDropped > 0) {
        printTs(); Log.printf("[espnow] %u frame(s) dropped — queue was full\n", s_qDropped);
        s_qDropped = 0;
    }
}

void espnowSendNodeOnline() {
    Msg m = makeMsg(CMD_NODE_ONLINE);
    esp_now_send(MAC_BROADCAST, reinterpret_cast<const uint8_t*>(&m), sizeof(Msg));
    s_onlineSentMs = millis(); if (!s_onlineSentMs) s_onlineSentMs = 1;
    printTs(); Log.println("[espnow] Sent NODE_ONLINE");
}

uint32_t espnowLastOnlineMs() { return s_onlineSentMs; }

EspNowStats espnowStats() {
    EspNowStats st;
    st.txOk = s_txOk; st.txFail = s_txFail;
    st.rxAll = s_rxAll; st.rxBadLen = s_rxBadLen; st.rxBadSeal = s_rxBadSeal; st.rxNotUs = s_rxNotUs;
    st.rxQueued = s_rxQueued; st.qDropped = s_qDroppedTotal; st.lastScarecrowMs = s_lastScarecrowMs;
    for (int i = 0; i < 6; i++) st.lastBadMac[i] = s_badMac[i];
    st.lastBadLen = s_badLen;
    uint8_t ch = 0; wifi_second_chan_t sc;
    st.channel = esp_wifi_get_channel(&ch, &sc) == ESP_OK ? ch : 0;
    return st;
}

// PIR saw movement (2026-09-25). Same frame as Spooky Tree's (CMD_PROP_MOTION,
// payload_a = this node). Informational only: the Scarecrow's tree aside only
// reacts to payload_a == NODE_SPOOKY_TREE (fleet.cpp), so nothing acts on this
// one yet. One frame, sent 3x (same seq) so a receiver can dedupe.
void espnowSendPropMotion() {
    Msg m = makeMsg(CMD_PROP_MOTION);
    m.payload_a = (uint32_t)NODE_PUMPKIN_MEN;
    m.payload_b = 1;  // 1 = motion detected
    msg_seal(m);      // reseal with updated payload
    for (int i = 0; i < 3; i++) {
        esp_now_send(MAC_BROADCAST, reinterpret_cast<const uint8_t*>(&m), sizeof(Msg));
        if (i < 2) delay(15);
    }
    printTs(); Log.println("[espnow] Sent PROP_MOTION (pumpkin men PIR) x3");
}

// No periodic NODE_ONLINE any more (2026-10-01, ported from the Spooky Tree's
// 2026-09-27 fix): it means "I just booted", and the Scarecrow answers each one
// with a theme + SHOW_NARRATOR resync burst for its last session. Every 5 min
// that put this node in a fake visit (PIR actions held, a running action cut).
// Sent at boot only. Kept as a no-op so loop() needn't change.
void espnowHeartbeatTick() {}
