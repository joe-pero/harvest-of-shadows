#pragma once

#include <Arduino.h>
#include "pir_logic.h"

// 12 V alarm PIR via a relay (2026-09-27): COM = GND, NO = GPIO27, internal
// pull-up. HIGH = idle, LOW = motion. Logic + bench data: pir_logic.h.
#define PIR_PIN          27
#define PIR_COOLDOWN_MS  120000  // 2 minutes quiet after an action track ends (Joe, 2026-09-27)
#define PROP_MOTION_COOLDOWN_MS 180000  // PROP_MOTION to the Scarecrow: at most once per 3 min (Joe, 2026-09-27)

void pirInit();

// Call every loop, in every play state (the old pirCheck() only ran during the
// ambient and kept a stale HIGH across themes = a false trip right after).
pirlogic::Ev pirTick(uint32_t now);
const pirlogic::PirEdge& pirEdge();
pirlogic::PirGate& pirGate();
