#pragma once
// =============================================================================
// ESP-NOW for the touchpad controller — RX listener + broadcast origination
// =============================================================================
// Same broadcast-only mesh contract as props/spooky-tree: WiFi STA, channel
// locked to WIFI_CHANNEL (no AP association), broadcast peer, 8-slot inbound
// ring buffer drained from loop().
//
// Unlike the pure listeners (patch audio / singing pumpkins), this node also
// ORIGINATES broadcasts — CMD_OTA_MODE, the zombie CMD_SEGMENT_START/_END, and
// (future) god-mode overrides — via espnowBroadcastCmd().
// =============================================================================

#include <Arduino.h>
#include "ProjectConstants.h"

typedef void (*EspNowCmdCallback)(const Msg&);

void espnowInit();
void espnowSetCallback(EspNowCmdCallback cb);
void espnowTick();                 // drain the whole inbound queue each loop
void espnowSendNodeOnline();
void espnowHeartbeatTick();         // CMD_NODE_ONLINE every 5 min

// Broadcast one command, sent `repeats` times ~200 ms apart (H1 pattern).
// session_id 0 = use the last one we heard.
void espnowBroadcastCmd(uint8_t cmd, uint32_t payload_a, uint32_t payload_b,
                        uint32_t session_id, uint8_t repeats = 3);

uint32_t espnowLastSession();
