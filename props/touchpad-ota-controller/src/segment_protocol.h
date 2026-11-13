#pragma once
// =============================================================================
// Zombie show-segment commands — LOCAL copy, mirrors
// Master Halloween Show 2026/nodes/zombie_container/src/zombie_protocol.h
// =============================================================================
// Rides the existing 22-byte Msg (ProjectConstants.h) — no new wire format.
// PROMOTE these into ProjectConstants.h's Command enum + a shared header when
// the pattern is exercised (see docs/zombie-container.md open items). Kept local
// here so this node builds without touching the shared header yet.
//
// The Eave Coordinator's Z_RED/GREEN/WHITE phase durations should be promoted
// alongside these so the panel + Eave share one source.
// =============================================================================

#include <stdint.h>

enum SegmentCommand : uint8_t {
    CMD_SEGMENT_START = 0x09,  // broadcast; payload_a = SegmentID; payload_b =
                               //   phase hint (0 red / 1 green / 2 strobe) on
                               //   per-phase re-broadcasts, else advisory dur_s
    CMD_SEGMENT_END   = 0x0A,  // broadcast; payload_a = SegmentID
};

enum SegmentID : uint8_t {
    SEGMENT_ZOMBIE = 1,        // green(idle) -> RED -> STROBE -> green(idle)
};

// payload_b phase-hint values on a CMD_SEGMENT_START re-broadcast
enum SegmentPhaseHint : uint8_t {
    SEGPHASE_RED    = 0,
    SEGPHASE_GREEN  = 1,
    SEGPHASE_STROBE = 2,
    SEGPHASE_DONE   = 3,
};
