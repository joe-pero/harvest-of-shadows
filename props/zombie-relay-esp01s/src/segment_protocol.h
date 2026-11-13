#pragma once
// =============================================================================
// Zombie show-segment commands — LOCAL copy, mirrors
// props/touchpad-ota-controller/src/segment_protocol.h (which itself mirrors
// Master Halloween Show 2026/nodes/zombie_container/src/zombie_protocol.h).
// =============================================================================
// Rides the existing 22-byte Msg (ProjectConstants.h) — no new wire format.
// This is now a THIRD copy of the same two enums (touchpad, Eave, this node).
// PROMOTE all three into ProjectConstants.h's Command enum + a shared header
// — tracked in docs/zombie-container.md open items; deferred again here on
// 2026-09-21 to keep this session's change minimal and not touch already-
// working touchpad/Eave firmware while debugging live hardware.
// =============================================================================

#include <stdint.h>

enum SegmentCommand : uint8_t {
    CMD_SEGMENT_START = 0x09,  // broadcast; payload_a = SegmentID
    CMD_SEGMENT_END   = 0x0A,  // broadcast; payload_a = SegmentID
};

enum SegmentID : uint8_t {
    SEGMENT_ZOMBIE = 1,
};
