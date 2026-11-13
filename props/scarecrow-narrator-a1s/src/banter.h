#pragma once
// =============================================================================
// banter -- Scarecrow CONDUCTS one OUTRO banter exchange across both boards
// docs/DESIGN-outro-banter-playback.md (approved 2026-09-24)
//
// Line by line: Scarecrow plays his own lines from /scarecrow/banter/<pool>/;
// for each of Burt's lines he cues Burt with the exact turn and waits for
// Burt's DONE for THAT turn (bounded by the line's duration from the card's
// index.csv + margin). Any failed turn STOPS the exchange (never play a reply
// to a line nobody heard). Burt must answer a presence probe before an
// exchange that needs him starts at all. Every outcome is logged and kept in
// banter_last() for the leader/status report: no silent failure.
// =============================================================================
#include <Arduino.h>
#include <stdint.h>
#include "ProjectConstants.h"

struct BanterResult {
    int8_t      pool;        // -1 = none attempted
    uint16_t    idx;
    uint8_t     total;       // lines in the exchange (0 = not found)
    uint8_t     played;      // lines actually heard
    const char* outcome;     // "complete" | "no_index" | "not_on_card" | "burt_unreachable"
                             // | "sc_file_missing" | "burt_failed" | "burt_timeout"
};

// Run one exchange. `session` must be the current show session (Burt's DONE is
// matched against it).
BanterResult banter_run(uint8_t pool, uint16_t idx, ShowTheme theme, ShowMode tone,
                        uint32_t session);

BanterResult banter_last();
