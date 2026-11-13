#pragma once
// =============================================================================
// Non-blocking momentary relay pulse
// =============================================================================
// relay_setup() puts the pins in the de-asserted (safe) state.
// relay_pulse(pin, ms) asserts the relay and schedules its release.
// relay_tick() releases any relay whose pulse window has elapsed — call every loop.
//
// Fail-safe: on any hang the relays are simply never re-asserted; a pulse in
// flight still releases on schedule because release is time-based, not event-based
// (and a full hang that stops relay_tick() leaves the contact closed only until
// the next reboot — acceptable for a trigger input, never a load).
// =============================================================================

#include <Arduino.h>

void relay_setup();
void relay_pulse(uint8_t pin, uint32_t ms);
void relay_tick();
bool relay_busy();                 // any pulse currently in flight
void relay_release_all();          // e-stop — force every relay de-asserted now
