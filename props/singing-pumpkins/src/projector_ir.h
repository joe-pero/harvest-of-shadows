#pragma once
// =============================================================================
// Projector power over IR (Joe 2026-10-01). Hitachi POWER, NEC 0xE1A2E817
// (docs/projector-ir.md). IR is the FALLBACK for a projector whose serial
// control doesn't work right.
//
// IR LED module S -> GPIO17 (TX2). VCC 3V3/5V, GND. The beam is narrow: aim it
// straight at the projector's IR window.
//
// ON:  one POWER press PROJECTOR_BOOT_DELAY_MS after the ESP32 powers up (the
//      projector needs mains + standby first). Skipped if NTP says it's inside
//      the night-off window, so a reboot at 10 PM doesn't light the projector.
// OFF: nightly, local time (NTP, Pacific): 9:30 PM Fri + Sat nights, 8:30 PM
//      Sun-Thu, 11:00 PM on Oct 31. OFF_PRESSES POWER presses, OFF_PRESS_GAP_MS apart (Hitachi asks
//      "power off?" and needs a 2nd press to confirm; NOT yet verified on the
//      real projector). Skipped (logged) if the clock never synced.
// IR can't read projector state: a projector already off at the off time gets
// the same presses.
// Manual: USB serial "IRPWR" (one press), or the HA "Projector power" button.
// =============================================================================
#include <stdint.h>

void projector_ir_begin();      // in setup(), after WiFi starts; schedules the power-up press
void projector_ir_tick();       // every loop
void projector_ir_power();      // send POWER now (one frame, manual)
const char* projector_ir_last();   // "OFF sent (2 presses) 2026-10-20 20:30" etc., for HA
uint32_t projector_ir_version();   // bumps on every change of projector_ir_last()
