#pragma once

#include <stdint.h>

// Spooky Tree motion logic, Arduino-free so props/_tests can run it on the PC.
//
// Sensor (2026-09-27): 12 V alarm PIR (BRJ-201W) -> its 12 V light output drives
// a relay in the tree; relay COM = GND, NO = GPIO27, INPUT_PULLUP. So the line is
// HIGH at rest and LOW while the relay is closed. Bench over the bridge:
//   - one pass = one ~3.4 s LOW (the sensor's 3 s delay jumper)
//   - someone lingering = back-to-back ~3.3 s LOWs with < 2 s gaps, or one long
//     LOW (65 s seen at high sensitivity; Joe then turned sensitivity down)
//   - relay bounce on close/open = 1-160 blips, all < 5 ms
//   - idle line: zero edges (the 225 Hz noise of the direct wiring is gone)
//
// PirEdge debounces the line into TRIP / RELEASE / FAULT events. A TRIP is a
// fresh close: a relay held closed never trips twice, so a stuck relay or a
// crowd standing there can't re-fire it. PirGate is the cooldown.

namespace pirlogic {

enum class Ev : uint8_t { NONE, TRIP, RELEASE, FAULT };

struct PirEdge {
    uint32_t tripMs    = 300;      // LOW this long (continuous) = relay closed
    uint32_t releaseMs = 200;      // HIGH this long (continuous) = relay open again
    uint32_t faultMs   = 300000;   // closed this long = logged once as a fault

    bool     rawLow   = false;     // last sample
    uint32_t rawSince = 0;         // when the raw level last changed
    bool     closed   = false;     // debounced state
    uint32_t closedAt = 0;         // start of the LOW that tripped
    uint32_t heldMs   = 0;         // valid after RELEASE: how long it was closed
    bool     faulted  = false;

    // Call every loop with the current line level. At most one event per call.
    Ev update(bool lineLow, uint32_t now) {
        if (lineLow != rawLow) { rawLow = lineLow; rawSince = now; }
        const int32_t steady = (int32_t)(now - rawSince);
        if (!closed) {
            if (rawLow && steady >= (int32_t)tripMs) {
                closed = true; closedAt = rawSince; faulted = false;
                return Ev::TRIP;
            }
        } else if (!rawLow) {
            if (steady >= (int32_t)releaseMs) {
                closed = false; heldMs = rawSince - closedAt;
                return Ev::RELEASE;
            }
        } else if (!faulted && (int32_t)(now - closedAt) >= (int32_t)faultMs) {
            faulted = true;
            return Ev::FAULT;
        }
        return Ev::NONE;
    }
};

// Cooldown: after a trip is acted on, the next one is allowed cooldownMs after
// the reaction is over (the action track's end), like Pumpkin Men. While the
// reaction is still running, nothing is allowed.
struct PirGate {
    uint32_t cooldownMs = 180000;
    bool     busy       = false;   // acted on, reaction still running
    bool     cooling    = false;
    uint32_t readyAt    = 0;

    bool ready(uint32_t now) const {
        if (busy) return false;
        return !cooling || (int32_t)(now - readyAt) >= 0;
    }
    // ms until ready (0 = ready, UINT32_MAX = reaction still running)
    uint32_t leftMs(uint32_t now) const {
        if (busy) return UINT32_MAX;
        if (!cooling) return 0;
        const int32_t d = (int32_t)(readyAt - now);
        return d > 0 ? (uint32_t)d : 0;
    }
    void acted()                  { busy = true; }
    void reactionDone(uint32_t now) {
        if (!busy) return;
        busy = false; cooling = true; readyAt = now + cooldownMs;
    }
};

}  // namespace pirlogic
