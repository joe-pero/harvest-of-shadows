#pragma once
// =============================================================================
// ESP-NOW receive-only listener — spatial audio patch node (Tier 1)
// =============================================================================
// Rebuilt 2026-10-01 from the Spooky Tree's hardened handler (props/spooky-tree/
// src/espnow_handler.*): WiFi STA, channel locked to WIFI_CHANNEL (no AP),
// broadcast peer, 32-slot inbound ring buffer drained from loop(), init retried
// every 30 s, receive counters for the black box. This node never originates a
// fleet cue: it sends only CMD_NODE_ONLINE, once at boot (a periodic one makes
// the Scarecrow answer with a NARRATOR resync = fake visits on every follower).
// =============================================================================

#include <Arduino.h>
#include "ProjectConstants.h"

typedef void (*EspNowCmdCallback)(const Msg&);

void     espnowInit();
void     espnowSetCallback(EspNowCmdCallback cb);
void     espnowTick();               // drain the whole inbound queue each loop
void     espnowSendNodeOnline();     // boot (and after a radio recovery) only
uint32_t espnowLastOnlineMs();       // millis() of the last NODE_ONLINE sent (0 = never)
void     espnowHeartbeatTick();      // radio upkeep: retries a failed init every 30 s

struct EspNowStats {
    bool     ready;                  // radio up (init + broadcast peer OK)
    uint32_t txOk, txFail, chRelocks;
    uint32_t rxAll, rxBadLen, rxBadSeal, rxNotUs, rxQueued, qDropped;
    uint32_t lastScarecrowMs;        // millis() of the last valid Scarecrow frame (0 = never)
    uint8_t  lastBadMac[6];          // sender of the last wrong-size / bad-seal frame
    int      lastBadLen;
    uint8_t  channel;                // radio channel right now (0 = query failed)
};
EspNowStats espnowStats();
