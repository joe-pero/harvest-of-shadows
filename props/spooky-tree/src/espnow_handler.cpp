#include "espnow_handler.h"
#include "BlackBox.h"
#include "wifi_credentials.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>

static EspNowCmdCallback s_callback        = nullptr;

// Inbound ring buffer — single producer (WiFi/ESP-NOW recv task, commonRecvCb),
// single consumer (espnowTick() from loop()). Head/tail as volatile is sufficient
// for this SPSC pattern without a mutex, matching the rest of this codebase's
// no-RTOS-primitives style. Sized for a full SHOW_LOCK+SET_THEME+SET_MODE burst
// plus headroom; see IMPROVEMENTS.md 1.1.
// 32 (was 8, 2026-09-26): one Scarecrow theme burst is 9 frames in ~0.5 s and
// loop() can block 1-3 s in an MP3 start/poll; plus other nodes' broadcasts.
#define ESPNOW_QUEUE_SIZE 32
static Msg              s_queue[ESPNOW_QUEUE_SIZE] = {};
static volatile uint8_t s_qHead                    = 0;  // next slot to read
static volatile uint8_t s_qTail                    = 0;  // next slot to write
static uint16_t         s_qDropped                 = 0;  // queue-full drop count (diagnostic)

static uint32_t          s_sessionId       = 0;
static uint16_t          s_seq             = 0;

static void printTs() {
    uint32_t ms = millis();
    Log.printf("[%02u:%02u.%03u] ", ms / 60000, (ms % 60000) / 1000, ms % 1000);
}

// ── IDF 4/5 receive callback bridge ──────────────────────────────────────────
// From docs/harvest_notes/espnow_findings.md — IDF 4.x / 5.x Compatibility Bridge

// Receive counters (2026-10-01 black box: is the radio part of the silence?).
// Written on the WiFi task, read by the loop's status line; plain volatile
// counters, a torn read only skews one status line.
static volatile uint32_t s_rxAll = 0, s_rxBadLen = 0, s_rxBadSeal = 0, s_rxNotUs = 0, s_rxQueued = 0;
static volatile uint32_t s_qDroppedTotal = 0, s_lastRxMs = 0, s_lastScarecrowMs = 0;
static volatile uint8_t  s_badMac[6] = {};
static volatile int      s_badLen = 0;

static void commonRecvCb(const uint8_t* mac, const uint8_t* data, int len) {
    s_rxAll++;
    s_lastRxMs = millis();
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
    if (m.target != (uint8_t)NODE_BROADCAST && m.target != (uint8_t)NODE_SPOOKY_TREE) { s_rxNotUs++; return; }

    uint8_t next = (uint8_t)((s_qTail + 1) % ESPNOW_QUEUE_SIZE);
    if (next == s_qHead) {
        // Queue full — drop the arriving frame rather than overwrite an unprocessed
        // one. Under normal show traffic this should never happen; a rising count
        // means espnowTick() isn't being called often enough (e.g. stuck elsewhere
        // in loop()).
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

// Broadcast TX result (no receiver ACK on broadcast: this says the frame left
// the radio, not that the Scarecrow got it). Runs on the WiFi task.
static volatile uint32_t s_txOk = 0, s_txFail = 0;
static void onSendStatus(const uint8_t* /*mac*/, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) s_txOk++; else s_txFail++;
}

static uint32_t s_onlineSentMs = 0;     // last NODE_ONLINE (0 = never)
static bool     s_ready       = false;  // esp_now_init + broadcast peer OK
static uint32_t s_lastInitTry = 0;
static uint32_t s_chRelocks   = 0;
static uint32_t s_motionSent  = 0, s_motionQueueFail = 0;

// ── Outbound message factory ──────────────────────────────────────────────────

static Msg makeMsg(Command cmd) {
    Msg m = {};
    m.command    = (uint8_t)cmd;
    m.sender     = (uint8_t)NODE_SPOOKY_TREE;
    m.target     = (uint8_t)NODE_BROADCAST;
    m.flags      = 0;
    m.session_id = s_sessionId;
    m.seq        = s_seq++;
    msg_seal(m);
    return m;
}

// ── Public API ────────────────────────────────────────────────────────────────

static bool espnowStart();

void espnowInit() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);           // no modem-sleep — keep ESP-NOW RX latency low
    esp_wifi_set_ps(WIFI_PS_NONE);  //   (docs/espnow_audit.md 4.1)

    // No AP connection — ESP-NOW only. Lock radio to configured channel.
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    printTs(); Log.printf("[espnow] Channel locked to %d\n", WIFI_CHANNEL);

    s_sessionId = esp_random();
    espnowStart();
}

// esp_now_init + callbacks + broadcast peer. Retried from espnowHeartbeatTick()
// every 30 s if it fails, so a bad boot doesn't leave the tree deaf and mute.
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

    // Broadcast-only architecture — no 20-peer limit, fire-and-forget,
    // show continues if any node is offline. Filter by msg.target in handler.
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

// The tree never joins an AP, so nothing should move it off the fleet channel —
// but a wrong channel = every frame lost with no error. Check before each send.
static void ensureChannel() {
    uint8_t ch = 0; wifi_second_chan_t sc;
    if (esp_wifi_get_channel(&ch, &sc) == ESP_OK && ch == WIFI_CHANNEL) return;
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    s_chRelocks++;
    printTs(); Log.printf("[espnow] !! channel drift: was %u, re-locked to %d (x%lu)\n",
                             (unsigned)ch, WIFI_CHANNEL, (unsigned long)s_chRelocks);
}

void espnowSetCallback(EspNowCmdCallback cb) {
    s_callback = cb;
}

void espnowTick() {
    while (s_qHead != s_qTail) {
        Msg m = s_queue[s_qHead];  // copy out before advancing head — safe against
        s_qHead = (uint8_t)((s_qHead + 1) % ESPNOW_QUEUE_SIZE);  // a concurrent producer write
        if (s_callback) s_callback(m);
    }
    if (s_qDropped > 0) {
        printTs(); Log.printf("[espnow] %u frame(s) dropped — queue was full\n", s_qDropped);
        s_qDropped = 0;
    }
}

void espnowSendNodeOnline() {
    if (!s_ready) { printTs(); Log.println("[espnow] NODE_ONLINE not sent — radio not up"); return; }
    ensureChannel();
    Msg m = makeMsg(CMD_NODE_ONLINE);
    esp_now_send(MAC_BROADCAST, reinterpret_cast<const uint8_t*>(&m), sizeof(Msg));
    s_onlineSentMs = millis(); if (!s_onlineSentMs) s_onlineSentMs = 1;
    printTs(); Log.println("[espnow] Sent NODE_ONLINE");
}

uint32_t espnowLastOnlineMs() { return s_onlineSentMs; }

// PIR saw movement (2026-09-24). Its own command, CMD_PROP_MOTION — not
// CMD_TRIGGER, which meant "start the show" to the retired Main Switch path and
// could start Pumpkin Men. Informational only: the Scarecrow is meant to react
// to it later. One frame, sent 3x (same seq) so a receiver can dedupe.
// Hardened 2026-09-27: the 3 copies are spread over 180 ms (was 30 ms: one
// burst of interference could take all three), every esp_now_send() result is
// checked, and the channel is verified first. Same Msg (same seq) each time.
// The Scarecrow keeps only the latest arrival time and speaks 1.5 s after it
// (aside.cpp), so the spread only moves its line by <= 0.18 s.
static const uint16_t MOTION_GAPS_MS[] = { 60, 120 };   // 0, 60, 180 ms
uint8_t espnowSendPropMotion() {
    if (!s_ready) {
        s_motionQueueFail++;
        printTs(); Log.println("[espnow] !! PROP_MOTION NOT sent — radio not up (init failed)");
        return 0;
    }
    ensureChannel();
    Msg m = makeMsg(CMD_PROP_MOTION);
    m.payload_a = (uint32_t)NODE_SPOOKY_TREE;
    m.payload_b = 1;  // 1 = motion detected
    msg_seal(m);      // reseal with updated payload
    uint8_t queued = 0;
    esp_err_t lastErr = ESP_OK;
    for (int i = 0; i < 3; i++) {
        esp_err_t e = esp_now_send(MAC_BROADCAST, reinterpret_cast<const uint8_t*>(&m), sizeof(Msg));
        if (e == ESP_OK) queued++; else lastErr = e;
        if (i < 2) delay(MOTION_GAPS_MS[i]);
    }
    s_motionSent++;
    if (queued == 3) {
        printTs(); Log.printf("[espnow] Sent PROP_MOTION (tree PIR) x3 over 180 ms, seq=%u\n", (unsigned)m.seq);
    } else {
        s_motionQueueFail++;
        printTs(); Log.printf("[espnow] !! PROP_MOTION only %u/3 queued (last err 0x%X)\n",
                                 (unsigned)queued, (unsigned)lastErr);
    }
    return queued;
}

EspNowStats espnowStats() {
    EspNowStats st;
    st.ready = s_ready; st.txOk = s_txOk; st.txFail = s_txFail;
    st.motionSent = s_motionSent; st.motionFail = s_motionQueueFail; st.chRelocks = s_chRelocks;
    st.rxAll = s_rxAll; st.rxBadLen = s_rxBadLen; st.rxBadSeal = s_rxBadSeal; st.rxNotUs = s_rxNotUs;
    st.rxQueued = s_rxQueued; st.qDropped = s_qDroppedTotal;
    st.lastRxMs = s_lastRxMs; st.lastScarecrowMs = s_lastScarecrowMs;
    for (int i = 0; i < 6; i++) st.lastBadMac[i] = s_badMac[i];
    st.lastBadLen = s_badLen;
    uint8_t ch = 0; wifi_second_chan_t sc;
    st.channel = esp_wifi_get_channel(&ch, &sc) == ESP_OK ? ch : 0;
    return st;
}

void espnowHeartbeatTick() {
    const uint32_t now = millis();
    if (!s_ready && (now - s_lastInitTry) >= 30000) {
        if (espnowStart()) espnowSendNodeOnline();
        return;
    }
    // No periodic NODE_ONLINE any more (2026-09-27): it means "I just booted",
    // and the Scarecrow answers each one with a theme + SHOW_NARRATOR resync
    // burst — every 5 min that kept this tree in a fake visit (action tracks
    // held). Sent at boot and after a radio recovery only. Nothing else used it.
}
