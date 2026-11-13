#pragma once
// =============================================================================
// MIDDLE delivery — push-then-play (docs/a1s-board-and-audio-architecture.md §2.2)
// =============================================================================
// The MIDDLE line is generated live per visitor on detect-server. The node:
//   1. builds the deterministic URL  <base>/middle/<session>_sc.wav
//   2. polls it (404/202 = "not ready") every ~500 ms until a deadline
//   3. on 200, downloads the WHOLE file into PSRAM, sanity-checks it
//   4. plays it (caller crossfades from the INTRO tail)
// No chunked streaming — generation, not transport, is the bottleneck, and a
// full download lets a late/short/corrupt file fall to the bridge line cleanly.
// =============================================================================

#include <Arduino.h>
#include <stdint.h>

enum MiddleResult : uint8_t {
    MID_PLAYED = 2,      // live MIDDLE fetched + played
    MID_PAD    = 1,      // deadline hit while still in flight — caller padded, then gave up
    MID_NONE   = 0,      // never became available / error — caller goes to bridge line
};

// base_url e.g. "http://192.168.1.x:8080" (no trailing slash).
// deadline_ms: wall-clock budget from now (arch: INTRO_len - 1s soft, +1.5s grace).
MiddleResult middle_fetch_and_play(const char* base_url, uint32_t session,
                                   uint32_t deadline_ms, volatile bool* abort_flag);

// =============================================================================
// Opportunistic single-check delivery — added 2026-09-18, replaces the
// blocking-wait model above for the REAL MQTT-cue path (docs/TASK-a1s-middle-
// parts-wiring.md, "THE ACTUAL PLAN (2026-09-18 revision)", Joe's design).
// MIDDLE/OUTRO are not waited on; they're checked for opportunistically at
// every natural cue checkpoint (end of INTRO, top of HOLD, top of MAIN-SHOW)
// without ever blocking that checkpoint's own timing.
// =============================================================================

// ONE HTTP GET attempt against <base_url>/middle/<session>_<suffix>.wav — no
// polling loop, no deadline. 200 => download (bounded, same shape as
// middle_fetch_and_play()'s inner download loop) + play now, return
// MID_PLAYED. Anything else (404/202/not-ready/error) => return MID_NONE
// IMMEDIATELY, no retry — the caller falls straight through to whatever it
// was already going to do next and tries again at a later checkpoint.
// `suffix` e.g. "sc" (MIDDLE) or "sc_outro" (the AI-outro payoff) — this
// board only ever checks its own ("sc"-prefixed) files; Burt's "bu"/
// "bu_outro" files live on his own board.
MiddleResult middle_check_and_play(const char* base_url, const char* suffix,
                                   uint32_t session, volatile bool* abort_flag);

// =============================================================================
// Pipelined (sentence-by-sentence) MIDDLE — added 2026-09-17, NOT YET
// BENCH-VERIFIED (built while Joe was away from hardware; flash + test before
// trusting live). Polls for a SEQUENCE of small files
// (<base_url>/middle/<session>_sc_part1.wav, _part2.wav, ...) instead of one
// big file — the live_tts.py service on the bridge writes each part to disk
// as soon as it's synthesized, well before later parts are done (Joe's
// "layering" idea: start playing sooner instead of waiting for the whole
// line). `tick_fn` is called every ~500ms during any wait so a long real
// deadline (up to ~90s for part 1, per Joe's own budget) doesn't starve
// mqtt_tick()/fleet_tick() the way one long blocking poll would — pass
// those in from main.cpp rather than middle.cpp taking on a direct
// dependency on mqtt_client.h/fleet.h.
//
// part1_deadline_ms: budget to wait for the FIRST part (covers the full
//   AI-call + first-sentence-synthesis time). next_part_deadline_ms:
//   budget for EACH part after that (should be much shorter — by the time
//   part 1 finishes playing, part 2 should already be ready or close, since
//   the bridge synthesizes it in the background while part 1 plays).
// Returns the number of parts actually played (0 = nothing arrived in time,
// caller should fall back to the bridge/generic pool line exactly as today).
uint32_t middle_fetch_and_play_parts(const char* base_url, uint32_t session,
                                     uint32_t part1_deadline_ms,
                                     uint32_t next_part_deadline_ms,
                                     void (*tick_fn)(),
                                     volatile bool* abort_flag);
