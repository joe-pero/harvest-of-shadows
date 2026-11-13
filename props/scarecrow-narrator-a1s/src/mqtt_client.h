#pragma once
// =============================================================================
// Real MQTT cue transport — replaces the serial/auto-cycle-only trigger.
// docs/a1s-board-and-audio-architecture.md §3.2, docs/fleet-communication-tiers.md
//
// Broker: Workshop's own mosquitto (192.168.1.x:1883, user "halloween" —
// same broker already used by Garage_Presence_System's relay_controller).
// Subscribes harvest/show/params (retained) + harvest/show/cue. Publishes
// harvest/show/leader/status (LWT "offline" / explicit "online", plus a
// periodic heartbeat/handshake rollup from main.cpp).
//
// Parsing (ArduinoJson) stays in mqtt_client.cpp — main.cpp only sees plain
// structs via the two registered callbacks.
// =============================================================================
#include <Arduino.h>
#include <stdint.h>

struct ShowParams {
    uint32_t session_id;
    uint8_t  theme;    // ShowTheme
    uint8_t  tone;     // ShowMode
    uint32_t cycle;
    // OUTRO banter (docs/DESIGN-outro-banter-playback.md, 2026-09-24):
    // chosen by the bridge. banter_pool = -1 -> no exchange (use OUTRO pool).
    int8_t   banter_pool;   // 0 scary, 1 friendly, 2 encore_scary, 3 encore_friendly
    uint16_t banter_idx;    // exchange number (1-based)
    uint8_t  banter_lines;  // line count, for logging/cross-check
    bool     encore;        // banter-only encore for a lingering group
    // 2026-09-25: will the bridge produce a MIDDLE for this session?
    // 1 yes, 0 no (AI off / out of hours / cap / failed read -> skip the
    // 30s post-INTRO wait), -1 key absent (older bridge: wait as before).
    int8_t   middle;
};

struct ShowCue {
    uint32_t session_id;
    char     phase[16];   // "preshow" | "hold" | "mainshow" | "encore" | "reset" | "nomiddle"
};

typedef void (*ShowParamsCallback)(const ShowParams&);
typedef void (*ShowCueCallback)(const ShowCue&);

void mqtt_begin();     // call once in setup(), after fleet_begin() brings WiFi up
void mqtt_tick();      // call every loop()
bool mqtt_connected();

void mqtt_set_params_callback(ShowParamsCallback cb);
void mqtt_set_cue_callback(ShowCueCallback cb);

// Publish a retained status payload to harvest/show/leader/status
// (main.cpp folds in fleet_print_handshake_log()'s rollup on a timer).
void mqtt_publish_status(const char* json);
// True if broker data is waiting unread on the socket. Local check only (no
// transmit). While the Scarecrow is idle the only unsolicited traffic is a
// params/cue publish, i.e. a show starting -- used to stop a tree aside at once.
bool mqtt_rx_pending();
