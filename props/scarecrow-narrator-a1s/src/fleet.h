#pragma once
// =============================================================================
// Fleet leader — WiFi STA (ch 11) + ESP-NOW bridge
// =============================================================================
// docs/a1s-board-and-audio-architecture.md Part 3 + docs/fleet-communication-tiers.md
//
//  - WiFi STA on channel 11 (every property AP is locked to 11 so ESP-NOW,
//    which rides the STA channel, always matches). setSleep(false) + PS_NONE.
//  - ESP-NOW: one broadcast peer (the whole fleet) + one unicast peer for Burt
//    (MAC_SKELETON_A1S) — the single Tier-2 relationship (§3.4 H1b).
//  - Fleet cues are BROADCAST, sent 3x ~200 ms apart, no custom ACK (H1).
//  - Scarecrow->Burt cue additionally unicasts CMD_NARRATOR_2_START and checks
//    the NATIVE ESP-NOW send-callback result: <=3 closely-spaced tries, then
//    proceed regardless. Never blocks the show (H1b).
//  - Leader heartbeat: CMD_HEARTBEAT every ~10 s, liveness only, carries NO show
//    state (2026-09-24; was SHOW_STATE(SHOW_IDLE), which told props the show
//    had ended mid-show). The Main Switch it once fed is retired.
// =============================================================================

#include <Arduino.h>
#include <stdint.h>
#include "ProjectConstants.h"

struct FleetHandshake {
    bool     success;      // native ESP_NOW_SEND_SUCCESS seen within the budget
    uint8_t  attempts;     // 1..3
};

struct FleetHandshakeStats {
    uint32_t cueOk;
    uint32_t cueFail;
    uint32_t cueAttempts;
    uint32_t burtAckRx;
};

void fleet_begin(uint32_t initial_session);
void fleet_tick();                       // call every loop()

bool fleet_wifi_up();
int  fleet_wifi_channel();
const char* fleet_ip();

void fleet_set_session(uint32_t session);

// ── leader broadcasts (H1: each is sent 3x internally) ──────────────────────
void fleet_broadcast_theme_burst(ShowTheme theme, ShowMode tone);   // SET_THEME+SET_MODE+SHOW_STATE(NARRATOR)
void fleet_broadcast_show_state(ShowState st);
void fleet_broadcast_start_scene(ShowTheme theme);
void fleet_broadcast_narrator_done(uint8_t which /*1|2*/);
void fleet_broadcast_node_online();

// ── H1b: cue Burt (unicast + native confirmation, <=3 tries, non-blocking) ──
// hold=true packs a HOLD indicator into a spare byte of payload_b (Burt's own
// firmware doesn't parse it yet — docs/TASK-a1s-hold-segment.md Part B,
// Burt-side follow-up) — Scarecrow's own HOLD line plays regardless.
FleetHandshake fleet_cue_burt(ShowTheme theme, ShowMode tone, bool hold = false);

// Poll-and-clear: true if Burt's CMD_NARRATOR_2_DONE broadcast arrived since
// the last call. Used to gate the post-narrator SHOW_ACTIVE transition on
// Burt's real completion instead of firing immediately.
bool fleet_narrator2_done();

// ── OUTRO banter turns (NarratorBanter.h, 2026-09-24) ─────────────────────
// Presence probe: unicast + native delivery confirmation, <=3 tries.
bool fleet_probe_burt();
// Arm BEFORE cueing, so a DONE that races back fast is still caught.
void fleet_arm_burt_turn(uint32_t session, uint32_t turnBits);
// Cue Burt for one banter line (broadcast + unicast, like fleet_cue_burt).
FleetHandshake fleet_cue_burt_turn(ShowTheme theme, uint32_t turnBits);
// Wait for Burt's DONE for exactly the armed turn.
//   1 = played, 0 = Burt reported failure (file missing), -1 = timeout.
int fleet_wait_burt_turn(uint32_t timeout_ms);

// Poll-and-clear: true if any node's CMD_NODE_ONLINE broadcast arrived since
// the last call (H4 reboot-resync — the leader answers with a fresh theme
// burst so a prop that just rebooted mid-cycle re-syncs).
bool fleet_node_online_seen();
// Spooky Tree PIR (CMD_PROP_MOTION): millis() of the latest report (0 = none yet),
// and how many frames were received (diagnostic).
uint32_t fleet_tree_motion_ms();
uint32_t fleet_tree_motion_count();

// diagnostics rollup for harvest/show/leader/status (populated by the calls above)
void fleet_print_handshake_log();
FleetHandshakeStats fleet_handshake_stats();

// ── heartbeat ──────────────────────────────────────────────────────────────
void fleet_heartbeat_tick();

// Burt's SD state from his last CMD_ACK: -1 unknown, 0 FAILED, 1 ok (2026-09-25)
int8_t fleet_burt_sd();

// ── coexistence self-test (§1.6 go/no-go for the leader role) ───────────────
// Hammers ESP-NOW broadcast while doing an HTTP GET from `url`, with optional
// audio playback active. Prints a pass/fail report. Returns true on pass.
bool fleet_coex_test(const char* url, bool with_playback);
