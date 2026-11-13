#pragma once
// =============================================================================
// Zombie Container DMX action-scene timeline — MEASURED
// =============================================================================
// From docs/zombie-container-dmx-timeline.md (Joe's screenshot analysis of the
// running PicoDMX show, 2026-09-07). All offsets are ms FROM THE TRIGGER.
//
// Structure:  (dark) -> GREEN open -> RED (with STROBE overlay) -> gap -> GREEN close -> idle
// Total show ~183.9 s.
//
// PROMOTE these (+ Eave's phase enum) into a shared header when the pattern is
// exercised — the panel LED and Eave must use one source. Eave's 2025
// Z_RED->Z_GREEN->Z_WHITE model is WRONG (green is start+end, strobe overlaps
// red) — rework it to this.
// =============================================================================

#include <stdint.h>

// ── macro phase boundaries, ms from trigger ─────────────────────────────────
// Show-state boundaries (Joe, 2026-09-07 + 2026-09-21 additions) — the HUD +
// LED + Eave all use these:
//   0 CONTAINED    0:00      .. 1:13.40   green swamp, safe readings (= night default)
//   1 WARNING      1:13.40   .. 1:20.10   red alert — container unstable, zombie hits, pipes, sparks
//   2 GLASS BREAK  1:20.10   .. 1:21.30   SOLID strobe — viewport fails, Trioxin gas release (1.2 s)
//   3 BROKEN       1:21.30   .. 1:40.00   specimen loose — broken electrical fire + red behind, garbage readings
//   4 DOUSE        1:40.00   .. 1:50.00   NEW 2026-09-21 — incineration fluid dumped on Specimen 04
//   5 INCINERATE   1:50.00   .. 2:49.86   NEW 2026-09-21 — fire purging the specimen
//   6 (CONTAINED)  2:49.86   .. 3:03.92   NEW 2026-09-21 — Joe: "feel free to do the contained safe
//                                         screen" for the tail; HS_RESET's distinct look is retired
//                                         from the live show (kept only for the bench `state reset`
//                                         command) — dmx_show_state() now returns HS_CONTAINED here.
static const uint32_t DMX_T_INTRO_DARK_MS      =  16000;   // 0:16  (video intro; lights dark before this)
static const uint32_t DMX_T_GREEN_OPEN_MS      =  73100;   // 1:13.10
static const uint32_t DMX_T_RED_START_MS       =  73400;   // 1:13.40  -> state 1 WARNING
static const uint32_t DMX_T_WARNING_START_MS   =  73400;   // alias
static const uint32_t DMX_T_STROBE_START_MS    =  80100;   // 1:20.10  -> state 2 GLASS BREAK
static const uint32_t DMX_T_GLASSBREAK_END_MS  =  81300;   // 1:21.30  -> state 3 BROKEN
static const uint32_t DMX_T_DOUSE_START_MS     = 100000;   // 1:40.00  -> state 4 DOUSE (Joe, 2026-09-21)
static const uint32_t DMX_T_INCINERATE_START_MS= 110000;   // 1:50.00  -> state 5 INCINERATE (Joe, 2026-09-21)
static const uint32_t DMX_T_STROBE_END_MS      = 160790;   // 2:40.79  last measured DMX strobe blip
static const uint32_t DMX_T_RED_END_MS         = 162760;   // 2:42.76
static const uint32_t DMX_T_GREEN_CLOSE_MS     = 169860;   // 2:49.86  -> back to CONTAINED (see above)
static const uint32_t DMX_T_END_MS             = 183920;   // 3:03.92  -> state 0 CONTAINED (loop)

// ── strobe overlay statistics (biased-random draw; matches the 46-blip data) ─
static const uint32_t DMX_STROBE_ON_TOTAL_MS = 22620;   // 22.62 s of ON over the window
static const float    DMX_STROBE_DUTY        = 0.281f;   // ~28 %
static const float    DMX_STROBE_FLASH_MIN_S = 0.13f, DMX_STROBE_FLASH_MAX_S = 2.70f;
static const float    DMX_STROBE_GAP_MIN_S   = 0.16f, DMX_STROBE_GAP_MAX_S   = 4.34f;  // longest observed gap 2:20.59->2:24.93
