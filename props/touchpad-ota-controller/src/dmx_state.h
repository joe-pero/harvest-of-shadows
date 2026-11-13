#pragma once
// =============================================================================
// DMX segment state model + RGB-LED indicator  (MEASURED timeline)
// =============================================================================
// The Zombie Container's DMX spotlight (on top, via a PicoDMX / Fright Ideas
// controller) runs a fixed ~184 s show:
//
//   (dark) -> GREEN open (~57 s) -> RED (~89 s, STROBE overlay ~80-161 s)
//          -> ~7 s gap -> GREEN close (~14 s) -> resting green "water" idle
//
// Exact offsets: dmx_timeline.h (from docs/zombie-container-dmx-timeline.md).
// This module tracks the phase (from the trigger) and renders it on the panel's
// RGB LED. It does NOT drive the fixture — the PicoDMX owns the show.
//
// ── timing source (see docs/zombie-container.md) ───────────────────────────
// The precise 46-blip strobe + sub-second phase flutters make it near-certain
// the timing lives in the PicoDMX scene. So the default is EXTERNAL: fire the
// trigger, then follow the measured clock. LOCAL is kept for the case where the
// ESP32 must manage it (re-broadcast per-phase hints / drive DMX) — same LED
// timeline either way.
// ── Eave reconcile ────────────────────────────────────────────────────────
// Eave's fw 2026-09-01-zombie1 uses Z_RED->Z_GREEN->Z_WHITE — WRONG per the
// measured data (green is start+end; strobe overlaps red). Rework Eave to this.
// =============================================================================

#include <Arduino.h>
#include "ProjectConstants.h"

enum DmxPhase : uint8_t {
    DMX_IDLE_GREEN = 0,   // resting green "water" (green + blue), no segment
    DMX_INTRO_DARK,       // 0 .. 16 s  (video intro, lights off)
    DMX_GREEN_OPEN,       // 16 s .. 1:13
    DMX_RED,              // 1:13 .. 2:43   (strobe overlays 1:20 .. 2:41)
    DMX_DARK_GAP,         // 2:43 .. 2:50
    DMX_GREEN_CLOSE,      // 2:50 .. 3:04  -> then back to DMX_IDLE_GREEN
};

enum DmxTimingMode : uint8_t { DMX_TIMING_EXTERNAL, DMX_TIMING_LOCAL };

void dmx_setup();
void dmx_set_timing_mode(DmxTimingMode m);
DmxTimingMode dmx_timing_mode();

void dmx_segment_start();                       // call after the relay pulse — t=0
void dmx_segment_start_at(uint32_t offset_ms);  // bench: start as if offset_ms already elapsed
void dmx_segment_end();                         // e-stop / CMD_SEGMENT_END / natural end

bool     dmx_segment_running();
DmxPhase dmx_phase();
uint32_t dmx_elapsed_ms();         // since dmx_segment_start(); 0 if idle
bool     dmx_strobe_window();      // within the strobe overlay window

void dmx_tick(uint32_t nowMs);     // advance phase + render the LED

// Bench: freeze on one state (ignoring the timeline) for tuning. -1 = normal.
//   0 CONTAINED  1 WARNING  2 GLASS BREAK  3 BROKEN  4 DOUSE  5 INCINERATE  6 RESET
void dmx_force_state(int s);
int  dmx_forced_state();

// Resolved show state for the LED + HUD (0..6 above). Honours a forced state;
// else derived from the elapsed time against the dmx_timeline.h boundaries.
// NOTE (2026-09-21): the live show never actually returns HS_RESET anymore —
// dmx_show_state() maps the 2:49.86-3:03.92 tail straight to HS_CONTAINED
// (Joe: "feel free to do the contained safe screen" for that window). HS_RESET
// is kept only for the bench `state reset` serial command.
int  dmx_show_state();
enum { HS_CONTAINED = 0, HS_WARNING, HS_GLASSBREAK, HS_BROKEN, HS_DOUSE, HS_INCINERATE, HS_RESET };

// 0..1 escalation ramp for HUD gauges: 0 contained, rising through warning,
// pegged in glass-break/broken, easing back down through reset.
float dmx_escalation();
