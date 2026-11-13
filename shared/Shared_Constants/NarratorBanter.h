#pragma once
// =============================================================================
// NarratorBanter.h -- OUTRO banter turn protocol, Scarecrow <-> Burt (A1S pair)
// docs/DESIGN-outro-banter-playback.md (approved 2026-09-24). ONE definition,
// compiled into both boards, so the two sides can never disagree on bits.
//
// Scarecrow conducts an exchange line by line. For each of Burt's lines it
// sends CMD_NARRATOR_2_START with payload_b = banter_turn(...). Burt plays
// exactly that clip and answers CMD_NARRATOR_2_DONE with payload_b =
// the SAME turn bits | DONE_PLAYED (or without it on failure). Scarecrow only
// accepts a DONE whose session + turn bits match the line it's waiting on, so
// a stale/duplicate DONE (Burt sends each 3x) can never start the next line
// early: no talking over each other.
//
// payload_b layout (CMD_NARRATOR_2_START):
//   bits 0-7   tone              (unchanged meaning)
//   bit  8     HOLD              (unchanged, never set on a banter turn)
//   bit  9     BANTER turn
//   bits 10-11 pool  (0 scary, 1 friendly, 2 encore_scary, 3 encore_friendly)
//   bits 12-15 line number within the exchange (1..4)
//   bits 16-31 exchange index (MANIFEST key, 1-based)
// CMD_NARRATOR_2_DONE.payload_b: the same bits 9-31 echoed, bit 0 = played.
// =============================================================================
#include <stdint.h>

static const uint32_t NARRATOR2_BANTER_BIT = 0x200;
static const uint32_t NARRATOR2_DONE_PLAYED = 0x1;
static const uint32_t NARRATOR2_TURN_MASK  = 0xFFFFFE00;   // bits 9-31

static const uint8_t BANTER_POOL_COUNT = 4;
static const char* const BANTER_POOL_NAMES[BANTER_POOL_COUNT] = {
    "scary", "friendly", "encore_scary", "encore_friendly"
};

// -1 if the name isn't a known pool.
static inline int banter_pool_id(const char* name) {
    if (!name) return -1;
    for (uint8_t i = 0; i < BANTER_POOL_COUNT; i++) {
        const char* a = BANTER_POOL_NAMES[i];
        const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return i;
    }
    return -1;
}

static inline uint32_t banter_turn(uint8_t tone, uint8_t pool, uint8_t line, uint16_t idx) {
    return (uint32_t)tone | NARRATOR2_BANTER_BIT | ((uint32_t)(pool & 0x3) << 10)
         | ((uint32_t)(line & 0xF) << 12) | ((uint32_t)idx << 16);
}
static inline bool     banter_is_turn(uint32_t b) { return (b & NARRATOR2_BANTER_BIT) != 0; }
static inline uint8_t  banter_pool(uint32_t b)    { return (b >> 10) & 0x3; }
static inline uint8_t  banter_line(uint32_t b)    { return (b >> 12) & 0xF; }
static inline uint16_t banter_idx(uint32_t b)     { return (uint16_t)(b >> 16); }
