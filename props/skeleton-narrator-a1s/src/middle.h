#pragma once
// =============================================================================
// Opportunistic MIDDLE-flavor/OUTRO delivery — Burt's side, added 2026-09-18/19
// (docs/_history/TASK-a1s-middle-parts-wiring.md, "Burt's side — same pattern"), built
// same night as Scarecrow's own opportunistic-check redesign, from the SAME
// design: a file is checked for opportunistically at a cue checkpoint, never
// waited on, never blocking that checkpoint's own timing.
//
// NOT YET BENCH-VERIFIED. This is Burt's FIRST-EVER HTTP request of any kind —
// everything on this board has been SD-card-only until now (WiFi STA is
// already up for OTA + the ESP-NOW link, so the network path itself is proven;
// only the HTTPClient usage is new). Verify on real hardware, don't assume it
// works because Scarecrow's identical function does — this project has hit
// real bugs specifically in Burt's own session-handling before (the uint32
// session_id overflow, the stale (session,phase) dedupe key).
// =============================================================================

#include <Arduino.h>
#include <stdint.h>

enum MiddleResult : uint8_t {
    MID_PLAYED = 2,      // file fetched + played
    MID_NONE   = 0,      // not ready / error — caller falls through to its own line
};

// ONE HTTP GET attempt against <base_url>/middle/<session>_<suffix>.wav — no
// polling loop, no deadline. 200 => download (bounded, 8s inner timeout) +
// play now, return MID_PLAYED. Anything else (404/202/not-ready/error) =>
// return MID_NONE IMMEDIATELY, no retry — the caller falls straight through
// to whatever it was already going to do next and tries again at a later
// checkpoint. `suffix` is "bu" (the flavor line) or "bu_outro" (the AI-outro
// payoff) — this board only ever checks its own ("bu"-prefixed) files;
// Scarecrow's "sc"/"sc_outro" files live on his own board.
MiddleResult middle_check_and_play(const char* base_url, const char* suffix,
                                   uint32_t session, volatile bool* abort_flag);
