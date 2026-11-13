#pragma once
// =============================================================================
// aside -- short idle-time Scarecrow reactions to the Spooky Tree's PIR
// (Joe, 2026-09-24: "who is walking by that spooky tree behind me?")
//
// The tree broadcasts CMD_PROP_MOTION (payload_a = NODE_SPOOKY_TREE) when its
// PIR fires. If the Scarecrow is IDLE (main.cpp decides: no visit latched, no
// SHOW_ACTIVE window, quiet since the last cue), it says one short line from
// /scarecrow/aside/tree/NNN.wav, at most once per cooldown.
//
// NEVER blocks the real show: the line is ~2-4 s and stops the instant any
// MQTT data is waiting (a show's params/cue), checked on the playing task
// between audio chunks. Adds NO radio traffic (listens to a broadcast the
// Scarecrow already receives; plays from SD; counters ride the existing
// leader/status publish), so no new WiFi/ESP-NOW contention.
// =============================================================================
#include <Arduino.h>
#include <stdint.h>

struct AsideStats {
    uint8_t  files;            // lines found on the card at boot
    uint32_t played;           // started (includes aborted)
    uint32_t aborted;          // stopped early because a show message arrived
    uint32_t skipped_busy;     // motion while the Scarecrow wasn't idle
    uint32_t skipped_cooldown; // motion inside the cooldown
    uint32_t skipped_stale;    // motion noticed too late to still be relevant
    uint32_t skipped_nofiles;  // motion but no aside lines on the card
};

// Count the aside lines on the card. Call once after pools_begin().
void aside_begin();

// Call every loop(). `idle` = nothing is happening with the show.
// `setPA` lets the aside switch the on-board amp on (and back off after).
void aside_tick(bool idle, bool paOn, void (*setPA)(bool));

// Bench: play one aside now (ignores motion/cooldown, still needs idle).
void aside_force();

AsideStats aside_stats();
