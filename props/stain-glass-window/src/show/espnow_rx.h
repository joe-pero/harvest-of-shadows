#pragma once
// Stain Glass show radio: receive-only ESP-NOW (Tier 1, docs/fleet-communication-tiers.md).
// Copied from props/spooky-tree/src/espnow_handler.* (proven in the 2026 show), minus PIR motion.
// The WiFi-task callback only queues; espnowRxTick() drains in loop() context.
#include <Arduino.h>
#include "ProjectConstants.h"

typedef void (*EspNowRxCallback)(const Msg&);

void espnowRxInit(EspNowRxCallback cb);   // STA, channel lock, esp_now_init, NODE_ONLINE once
void espnowRxTick();                      // drain queue + retry a failed init every 30 s

struct EspNowRxStats {
    bool ready;
    uint32_t rxAll, rxBadLen, rxBadSeal, rxNotUs, rxQueued, qDropped, chRelocks;
    uint32_t lastScarecrowMs;   // millis() of the last valid Scarecrow frame (0 = never)
    uint8_t channel;
};
EspNowRxStats espnowRxStats();
