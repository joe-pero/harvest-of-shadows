#pragma once
// =============================================================================
// INTRO / OUTRO / bridge pools on the microSD card
// =============================================================================
// Implements docs/a1s-board-and-audio-architecture.md §2.1 exactly:
//
//   /burt/
//     intro/  <theme>_<tone>/  000.wav .. NNN.wav          (6 buckets)
//     outro/  <theme>_<tone>/  000.wav .. NNN.wav          (6 buckets)
//     hold/   <theme>_<tone>/  000.wav .. NNN.wav          (6 buckets — taunt loop)
//     bridge/ generic/         000.wav .. NNN.wav          (1 flat pool)
//     index.csv                                            segment,bucket,count
//
// Selection is O(1): bucket = "<theme>_<tone>", n = table[seg][bucket],
// i = esp_random() % n, open "/burt/<seg>/<bucket>/%03d.wav". Anti-repeat
// ring of the last few indices per bucket. Missing file -> walk forward a few,
// then fall through to the bridge pool (§2.3 fallback ladder).
//
// The on-card sample rate can be anything the render emits (16k/22.05k/24k) —
// wav_play() reconfigures the codec per file.
// =============================================================================

#include <Arduino.h>
#include <stdint.h>
#include "ProjectConstants.h"

// HOLD appended before BRIDGE; BRIDGE renumbered so it stays a valid array index
// and existing mark_recent(SEG_BRIDGE,…) call sites keep working.
enum PoolSegment : uint8_t { SEG_INTRO = 0, SEG_OUTRO = 1, SEG_HOLD = 2, SEG_BRIDGE = 3 };

// Mount the SD card (1-bit, pins 14/15/2 — bench-confirmed DIP config) and read
// index.csv into RAM. Safe to call when no card is present: returns false and
// leaves the node in "no pools" state (caller uses the fallback ladder).
bool pools_begin();

bool     pools_mounted();
uint32_t pools_version();                 // pool_version line from index.csv, 0 if absent
int      pools_count(PoolSegment seg, ShowTheme theme, ShowMode tone);

// Play one line from the pool for (segment, theme, tone), applying the fallback
// ladder. Returns:
//   2 = played the requested bucket
//   1 = played a bridge/generic line (requested bucket unavailable)
//   0 = nothing played (no SD / bridge also unreadable) -> caller pads silence
// `abort_flag` (optional) interrupts playback mid-file.
int pools_play(PoolSegment seg, ShowTheme theme, ShowMode tone, volatile bool* abort_flag = nullptr);

// 2026-09-22: mirrors Scarecrow's pools_play_matched() (pools.h in
// scarecrow-narrator-a1s) -- session-derived index instead of esp_random(),
// so this board lands on the SAME OUTRO index as Scarecrow's for a given
// visit. See that file's comment for the full rationale + known edge case.
// NOT YET BENCH-VERIFIED.
int pools_play_matched(PoolSegment seg, ShowTheme theme, ShowMode tone,
                        uint32_t session, volatile bool* abort_flag = nullptr);

// Build the "<theme>_<tone>" bucket string, e.g. "rotld_scary". Buffer >= 20.
void pools_bucket_name(ShowTheme theme, ShowMode tone, char* out, size_t cap);

// Play one exact path (OUTRO banter line). 1 played, 0 missing/unreadable (logged).
int pools_play_file(const char* path, volatile bool* abort_flag = nullptr);
