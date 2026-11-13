#pragma once
// =============================================================================
// Burt link — WiFi STA (ch 11) + ESP-NOW follower side of the narrator pair
// =============================================================================
// Burt is a FOLLOWER (docs/fleet-communication-tiers.md T2-1). He does NOT
// originate fleet cues, does NOT talk to MQTT, is NOT the leader heartbeat.
// He:
//   - latches (theme, tone) off the fleet SET_THEME / SET_MODE broadcast
//     (and, belt-and-suspenders, off his own CMD_NARRATOR_2_START payload)
//   - waits for CMD_NARRATOR_2_START (broadcast OR the Scarecrow's unicast copy)
//   - on that cue: unicasts a CMD_ACK "starting" back to MAC_SCARECROW_A1S using
//     ESP-NOW's NATIVE send-callback result — <=3 closely-spaced tries, then
//     proceeds regardless (§3.4 H1b handoff direction; diagnostic only)
//   - plays his line, then broadcasts CMD_NARRATOR_2_DONE (3x)
// =============================================================================

#include <Arduino.h>
#include <stdint.h>
#include "ProjectConstants.h"

struct BurtCue {
    bool       pending;
    uint32_t   session;
    ShowTheme  theme;
    ShowMode   tone;
    bool       mainshow;     // false = pre-show line, true = OUTRO (a START_SCENE was seen)
    bool       hold;         // true = HOLD retrigger (TASK-a1s-hold-segment.md Part B) —
                              // packed into CMD_NARRATOR_2_START's payload_b upper byte by
                              // the Scarecrow (fleet.cpp), same theme/tone, no re-burst.
    // OUTRO banter turn (NarratorBanter.h, 2026-09-24): play exactly one line.
    bool       banter;
    uint32_t   turnBits;     // payload_b as received; echoed back in DONE
};

void burt_begin();
void burt_tick();

bool burt_wifi_up();
int  burt_wifi_channel();
const char* burt_ip();

// Non-blocking: returns true once when a fresh cue has arrived, fills `out`.
bool burt_take_cue(BurtCue& out);

// H1b handoff — unicast CMD_ACK "starting" to Scarecrow, native confirmation,
// <=3 tries, never blocks. Returns whether SUCCESS was seen.
bool burt_send_starting_ack(uint32_t session);

// Burt finished his line.
void burt_broadcast_narrator_done(bool mainshow);
// Banter turn finished: DONE echoes the turn bits (+ played flag) so the
// Scarecrow can match it to the exact line it's waiting on.
void burt_broadcast_turn_done(uint32_t turnBits, bool played);

void burt_print_log();
