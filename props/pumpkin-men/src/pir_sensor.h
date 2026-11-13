#pragma once

#include <Arduino.h>

// Motion sensor (Joe, 2026-09-25): the one PIR still wired on the box is the
// 2025 CLOSE sensor on GPIO36 (input-only, no internal pull — the PIR drives
// the line). The 2025 FAR (34) and MID (35) sensors are gone.
#define PIR_PIN          36
// Hardened 2026-09-26 (Joe: "rather it not play than over play"). Sensor is an
// HC-SR501, pipe-shrouded to a spot, pots at MAX sensitivity + MIN time: all
// filtering is here. Bench data: every real detection is one ~1.21 s HIGH
// pulse, and continuous motion gives a pulse every ~6 s (its lockout). A stray
// heat flicker gives one pulse; a person standing in the spot gives a train.
// So: a detection = PIR_PULSES_NEEDED valid pulses inside PIR_WINDOW_MS.
#define PIR_PULSES_NEEDED   3       // pulses needed …
#define PIR_WINDOW_MS   20000       // … within this window (sliding)
#define PIR_PULSE_MIN_MS  800       // shorter HIGHs are glitches, not detections
#define PIR_BLANK_MS     3000       // a pulse starting this soon after any output switch is ignored
#define PIR_WARMUP_MS   60000       // PIR settles after power-up (same as Spooky Tree)

void pirInit();

// True once when the pulse train qualifies (then the train starts over). No cooldown here:
// main.cpp re-arms after the action track ends (ACTION_COOLDOWN_MS).
// lastSwitchMs = relayLastSwitchMs(): HIGH periods that began inside the
// blanking window never count, however long they last.
bool pirCheck(uint32_t lastSwitchMs);
