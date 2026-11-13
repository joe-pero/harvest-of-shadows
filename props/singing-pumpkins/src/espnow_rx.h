#pragma once
// =============================================================================
// ESP-NOW receive-only listener — spatial audio patch node (Tier 1)
// =============================================================================
// Same broadcast-only mesh contract as props/spooky-tree/src/espnow_handler.*:
//   WiFi STA mode, channel locked to WIFI_CHANNEL (no AP association), ESP-NOW
//   broadcast peer, 8-slot inbound ring buffer drained from loop(). This node
//   never originates a fleet cue — it only listens (+ a NODE_ONLINE heartbeat).
// =============================================================================

#include <Arduino.h>
#include "ProjectConstants.h"

typedef void (*EspNowCmdCallback)(const Msg&);

void espnowInit();
void espnowSetCallback(EspNowCmdCallback cb);
void espnowTick();               // drain the whole inbound queue each loop
void espnowSendNodeOnline();
void espnowHeartbeatTick();       // CMD_NODE_ONLINE every 5 min
