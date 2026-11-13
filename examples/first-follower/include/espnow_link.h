// espnow_link.h: the minimum needed to talk on the show's ESP-NOW network.
//
//  * every board sits on ONE fixed Wi-Fi channel (WIFI_CHANNEL)
//  * every message is the 22-byte Msg from ProjectConstants.h, checked by magic, version and CRC
//  * messages are broadcast; each prop decides for itself what to act on
//
// Frames arrive in the Wi-Fi task, so the callback only copies them into a small queue;
// loop() takes them out with espnowNext(). Never do slow work inside the callback.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include "ProjectConstants.h"

#ifndef WIFI_CHANNEL
#define WIFI_CHANNEL 11      // must match every other board in the show (and your access point)
#endif

namespace espnow_link {

static const uint8_t QSIZE = 16;
static volatile uint8_t qHead = 0, qTail = 0;
static Msg queue[QSIZE];
static volatile uint32_t rxBad = 0, rxDropped = 0;
static uint16_t seq = 0;

static void onFrame(const uint8_t* data, int len) {
    if (len != (int)sizeof(Msg)) { rxBad++; return; }
    Msg m;
    memcpy(&m, data, sizeof(Msg));
    if (!msg_valid(m)) { rxBad++; return; }            // wrong magic / version / CRC: not ours
    uint8_t next = (uint8_t)((qTail + 1) % QSIZE);
    if (next == qHead) { rxDropped++; return; }         // queue full: count it, never block
    queue[qTail] = m;
    qTail = next;
}

#if ESP_IDF_VERSION_MAJOR >= 5
static void onRecv(const esp_now_recv_info_t*, const uint8_t* data, int len) { onFrame(data, len); }
#else
static void onRecv(const uint8_t*, const uint8_t* data, int len) { onFrame(data, len); }
#endif

// Start ESP-NOW on the show channel. Returns false (and says why) if anything fails.
inline bool begin() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);                               // no power-save: keeps receive latency low
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) { Serial.println("[espnow] init FAILED"); return false; }
    esp_now_register_recv_cb(onRecv);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, MAC_BROADCAST, 6);
    peer.channel = 0;                                   // 0 = "the current channel"
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(MAC_BROADCAST) && esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("[espnow] adding the broadcast peer FAILED");
        return false;
    }
    Serial.printf("[espnow] ready on channel %d, this board is %s\n", WIFI_CHANNEL, WiFi.macAddress().c_str());
    return true;
}

// Next received message, if any.
inline bool next(Msg& out) {
    if (qHead == qTail) return false;
    out = queue[qHead];
    qHead = (uint8_t)((qHead + 1) % QSIZE);
    return true;
}

// Broadcast one message (sealed with magic, version and CRC here).
inline bool send(uint8_t command, uint8_t sender, uint32_t session, uint32_t a, uint32_t b = 0) {
    Msg m = {};
    m.command = command;
    m.sender = sender;
    m.target = MSG_BROADCAST_TARGET;
    m.session_id = session;
    m.seq = seq++;
    m.payload_a = a;
    m.payload_b = b;
    msg_seal(m);
    return esp_now_send(MAC_BROADCAST, reinterpret_cast<const uint8_t*>(&m), sizeof(Msg)) == ESP_OK;
}

}  // namespace espnow_link
