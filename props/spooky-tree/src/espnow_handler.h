#pragma once

#include <Arduino.h>
#include "ProjectConstants.h"

// Called from espnowTick() for each validated inbound Msg.
// Registered by main.cpp via espnowSetCallback().
typedef void (*EspNowCmdCallback)(const Msg&);

// Initialise WiFi STA (no AP), lock the channel, start ESP-NOW + broadcast peer
// (retried every 30 s from espnowHeartbeatTick() if it fails).
void espnowInit();

void espnowSetCallback(EspNowCmdCallback cb);

// Drains every pending inbound message queued since the last call (deferred from
// the WiFi-task receive callback via a 32-slot ring buffer), dispatching each to
// the registered callback in arrival order. Call every loop iteration.
//
// Draining the whole queue in one pass (not one message per call) is what keeps a
// back-to-back burst — e.g. the Scarecrow's SET_THEME + SET_MODE + SHOW_STATE sent with
// no spacing — from losing frames to a single-slot overwrite (see IMPROVEMENTS.md
// 1.1). The callback itself must stay non-blocking (defer any mp3Stop()/
// mp3PlayTrack() to a flag main.cpp's loop() acts on after the drain) or a slow
// handler will still stall the queue mid-burst.
void espnowTick();

// Broadcast outbound messages.
void espnowSendNodeOnline();
uint32_t espnowLastOnlineMs();      // millis() of the last NODE_ONLINE sent (0 = never)
// PIR saw movement → CMD_PROP_MOTION (payload_a = NODE_SPOOKY_TREE), 3 copies
// over 180 ms. Returns how many copies the radio accepted (0 = none: logged).
uint8_t espnowSendPropMotion();

struct EspNowStats {
    bool     ready;                 // radio up (init + broadcast peer OK)
    uint32_t txOk, txFail;          // broadcast frames that left / failed to leave the radio
    uint32_t motionSent, motionFail;// PROP_MOTION reports / reports with a copy not queued
    uint32_t chRelocks;             // channel found off WIFI_CHANNEL and re-locked
    // Receive side (since boot). rxAll = every frame the radio handed us.
    uint32_t rxAll, rxBadLen, rxBadSeal, rxNotUs, rxQueued, qDropped;
    uint32_t lastRxMs, lastScarecrowMs; // millis() of the last frame / last valid Scarecrow frame (0 = never)
    uint8_t  lastBadMac[6];         // sender of the last wrong-size / bad-seal frame
    int      lastBadLen;
    uint8_t  channel;               // radio channel right now (0 = query failed)
};
EspNowStats espnowStats();

// Radio upkeep: retries a failed ESP-NOW start every 30 s (then NODE_ONLINE).
// No periodic NODE_ONLINE (see .cpp). Call every loop iteration.
void espnowHeartbeatTick();
