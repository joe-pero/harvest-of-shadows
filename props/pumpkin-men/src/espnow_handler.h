#pragma once

#include <Arduino.h>
#include "ProjectConstants.h"

// Called from espnowTick() for each validated inbound Msg.
// Registered by main.cpp via espnowSetCallback().
typedef void (*EspNowCmdCallback)(const Msg&);

// Initialise WiFi STA, connect to AP, start ESP-NOW, register broadcast peer.
void espnowInit();

void espnowSetCallback(EspNowCmdCallback cb);

// Drains every pending inbound message queued since the last call (deferred from
// the WiFi-task receive callback via a 32-slot ring buffer), dispatching each to
// the registered callback in arrival order. Call every loop iteration.
// The callback itself must stay non-blocking (see props/spooky-tree/IMPROVEMENTS.md
// 1.1 — this node's ESP-NOW layer is a direct port of Spooky Tree's).
void espnowTick();

// Broadcast outbound messages.
void espnowSendNodeOnline();
void espnowSendPropMotion();       // PIR saw movement → CMD_PROP_MOTION (payload_a = NODE_PUMPKIN_MEN)

// No-op since 2026-10-01 (NODE_ONLINE is boot-only now, see the .cpp).
void espnowHeartbeatTick();
uint32_t espnowLastOnlineMs();      // millis() of the last NODE_ONLINE sent (0 = never)

struct EspNowStats {
    uint32_t txOk, txFail;          // broadcast frames that left / failed to leave the radio
    // Receive side (since boot). rxAll = every frame the radio handed us.
    uint32_t rxAll, rxBadLen, rxBadSeal, rxNotUs, rxQueued, qDropped;
    uint32_t lastScarecrowMs;       // millis() of the last valid Scarecrow frame (0 = never)
    uint8_t  lastBadMac[6];         // sender of the last wrong-size / bad-seal frame
    int      lastBadLen;
    uint8_t  channel;               // radio channel right now (0 = query failed)
};
EspNowStats espnowStats();
