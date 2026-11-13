"""
Enums and timing constants — ported 1:1 from the show firmware so the
orchestration layer and the ESP32 nodes agree on vocabulary.

Sources (in ~/Documents/Halloween-Dev/PlatformIO/Master Halloween Show 2026/):
  ShowMode, ShowTheme          lib/Shared_Constants/ProjectConstants.h
  PIR_COOLDOWN_MS              nodes/main_switch_controller/src/inputs.h   (120000)
  NARRATOR_DEAD_MAN_MS         nodes/main_switch_controller/src/main.cpp   (300000)

Where a value here differs from the firmware it is called out — Show Logic
Spec v2 (2026-09-03) is the source of truth and the firmware needs to catch up.

TRIGGER GEOMETRY 2026-09-08 — `docs/show-trigger-geometry.md`. The Front Yard
East camera was repositioned and rezoned: two sidewalk PRIME zones, a
`waiting_zone` (pre-show trigger + photo point), and `brick_walkway`
(main-show trigger). `front_yard_west` is out of the trigger path. The sidewalk
pre-show / brick-walkway two-zone model this file used to encode is superseded.
"""
from __future__ import annotations

from enum import IntEnum


class ShowMode(IntEnum):
    """Tone axis. Matches Command-enum values on the wire."""
    FRIENDLY = 0
    SCARY = 1
    INTENSE = 2   # reserved; not used by the spec's tone rules yet


class ShowTheme(IntEnum):
    """The three coexisting worlds. One is locked-dominant per show cycle."""
    CORN = 0             # Children of the Corn
    ROTLD = 1            # Return of the Living Dead
    MUTANT_PUMPKINS = 2


THEME_LABEL = {
    ShowTheme.CORN: "Children of the Corn",
    ShowTheme.ROTLD: "Return of the Living Dead",
    ShowTheme.MUTANT_PUMPKINS: "Mutant Pumpkins",
}

# ── Frigate zones (cameras `front_yard_east` + `front_yard_west`) ────────────
# `docs/show-trigger-geometry.md` §1 (east, 2026-09-08) + 2026-09-21 update
# (west re-added as a second PRIME/PRE-SHOW angle: `zone_two` = PRIME,
# `zone_one` = PRE-SHOW, both additive alongside east's zones -- "either
# camera fires" per the original multi-camera redundancy plan in
# `docs/frigate-audit-report.md`). Live names as they appear in the Frigate
# config. Keep these in step with the box if the zones are ever renamed again.
SHOW_CAMERAS = frozenset({"front_yard_east", "front_yard_west"})
PRIME_ZONES = frozenset({"sidewalk_zone_1", "sidewalk_zone_2", "zone_two"})
WAITING_ZONES = frozenset({"waiting_zone", "zone_one"})
WALKWAY_ZONE = "brick_walkway"   # front_yard_east only -- the commit/main-show trigger

# ── Timing (seconds) ─────────────────────────────────────────────────────────

# PRIMED → IDLE if no waiting-zone trigger arrives. A prime is a disposable
# warm-up; a visitor who wanders past the sidewalk and leaves burns nothing.
PRIME_TIMEOUT_S = 90

# `waiting_zone` dwell before the pre-show fires. Enforced on the box by the
# zone's `loitering_time`; mirrored here so the sim matches. Only fire when the
# visitor actually STOPS at the wait spot — a straight walk-through is caught by
# the walkway trigger instead.
WAITING_ZONE_MIN_DWELL_S = 2

# Event-stream dedup window for the real `frigate/events` subscriber — collapse
# repeated `waiting_zone` enter events for one visit into a single logical
# trigger. Advisory for the live layer; the sim state machine never re-runs the
# pre-show on an active cycle regardless, so it doesn't consume this.
WAITING_ZONE_DEDUP_S = 5

# The "you're still standing there" loop (docs/show-trigger-geometry.md §2c).
# Gap between hold taunts. Joe's number 2026-09-08; needs real visitor-pacing
# data to confirm (cross-camera-zone-timing-TEST-DATA.md is geometry-only).
HOLD_INTERVAL_S = 40
# Taunts before the yard goes quiet and waits out CYCLE_IDLE_TIMEOUT_S.
HOLD_MAX_LOOPS = 3

# Safety reset if a cycle never reaches its main show (e.g. visitor leaves after
# pre-show and never enters the yard). Ported from NARRATOR_DEAD_MAN_MS (300000).
CYCLE_IDLE_TIMEOUT_S = 300

# Debounce for the "no active cycle" abbreviated walkway trigger (added
# 2026-09-17 after a live test: Frigate track-ID churn on one lingering
# visitor re-fired `walkway_trigger()` 4x in ~24s with zero gap, each firing
# a real MQTT cue -- but Scarecrow's own OUTRO playback (10-15s, fully
# blocking, no mqtt_tick() serviced meanwhile -- BUILD_LOG 2026-09-11) meant
# only the first 2 of 4 ever reached it; the rest silently vanished. This
# does NOT touch the documented "no cooldown" behavior for a walkway trigger
# during an ALREADY-ACTIVE cycle (controller.py, that's a different, correct
# immediate-response path) -- only the no-active-cycle walk-through branch.
# Originally set to 20s from Scarecrow's OUTRO length alone; raised to 50s
# the same night once Burt was actually put on the bench for the first time
# and his real lines measured at ~24-25s -- a full two-board cycle
# (Scarecrow OUTRO + Burt's response) is ~35-40s+, not ~15s, so 20s would
# still let a lingering visitor retrigger a new cycle while Burt was mid-line
# ("they talked over each other" -- Joe, live).
ABBREV_WALKWAY_COOLDOWN_S = 50

# Walkway ENCORE (Joe, 2026-09-24; docs/DESIGN-outro-banter-playback.md 3.5).
# A walkway trigger with no active cycle, within ENCORE_WINDOW_S of the last
# walkway trigger after a MAIN-SHOW, is the SAME group lingering on the bricks
# (Frigate re-fires on every track re-entry). Encore = banter only: same
# theme/tone, a fresh "still here?" exchange, no props/music, no deck draw.
# At most MAX_ENCORES per lingering group, at least ENCORE_MIN_GAP_S apart
# (longest exchange ~30s + margin); after the cap, quiet until the walkway has
# been clear for ENCORE_WINDOW_S.
ENCORE_WINDOW_S = 120
MAX_ENCORES = 2
ENCORE_MIN_GAP_S = ABBREV_WALKWAY_COOLDOWN_S
# 2026-09-28 hardening: hard ceiling on "same group", measured from the MAIN-SHOW.
# ENCORE_WINDOW_S slides on every walkway re-fire, so on a busy night with
# steady walkway traffic it never closed: every NEW group got banter-only
# encores, then silence after the cap. Past this age a walkway trigger is a
# new visit again (abbreviated cycle, its own 50s cooldown still applies).
ENCORE_GROUP_MAX_S = 240
# Joe 2026-09-28: a visit is over after 4 minutes -> normal reset. (Preshow
# ~15-45s + 3 HOLD taunts 40s apart fits well inside.)
VISIT_MAX_S = 240
# Live-bridge watchdog (loud safety net behind VISIT_MAX_S): a cycle older than
# this means the controller's own reset failed; force it closed.
MAX_CYCLE_AGE_S = 300

# Theme-music phase after the main-show OUTRO pair. CONFIRMED 2026-09-06
# (ProjectConstants.h SHOW_ACTIVE_MS); mirrored here per
# a1s-board-and-audio-architecture.md §3.2 ("add the constant to showsim").
SHOW_ACTIVE_MS = 210_000

# Tone used when the AI call is `uncertain`, times out, or fails. The
# fallback DEFAULT — the two-stage fallback RACE is gone (waiting-zone trigger
# has no latency deadline). Joe flips this if Friendly should be the safe call.
FALLBACK_TONE = ShowMode.SCARY

# Simulated narrator speaking duration per line (keeps the handoff visible in
# the log without real TTS). Not a spec value.
NARRATOR_LINE_SIM_S = 4

# Joe 2026-09-28: after a MAIN-SHOW the audio props play that theme for ~3.5
# min (SHOW_ACTIVE window). Any new cycle inside it (same group bouncing back
# into the waiting zone, a walk-through) REUSES that theme -- no deck draw --
# so the talking never fights the music. More talking is fine (Joe).
THEME_LOCK_S = SHOW_ACTIVE_MS // 1000

# Joe 2026-09-28: FREE show (AI off) = GATE CLOSED. Until Halloween the gate is
# shut and the waiting zone is the main viewing spot, so nobody reaches the
# brick walkway and the MAIN-SHOW never fired. In gate-closed mode the MAIN-SHOW
# plays from the waiting zone after this many HOLD taunts (1 -> ~80s after the
# PRE-SHOW) if they're still there. The walkway still triggers in every mode.
WAITING_MAINSHOW_AFTER_TAUNTS = 1
# ...and a group still standing there after the show gets encores (banter, up to
# MAX_ENCORES, this far apart, within ENCORE_GROUP_MAX_S of the show). Joe: the
# girls in the waiting zone wanted to hear him again.
LINGER_ENCORE_GAP_S = 60
