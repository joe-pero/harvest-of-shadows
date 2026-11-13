#!/usr/bin/env python3
"""
Tests for the ShowController state machine (docs/show-trigger-geometry.md).

Run: python3 test_controller.py   (plain stdlib, no pytest needed)
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from showsim.constants import (
    THEME_LOCK_S,
    ABBREV_WALKWAY_COOLDOWN_S,
    CYCLE_IDLE_TIMEOUT_S,
    HOLD_INTERVAL_S,
    HOLD_MAX_LOOPS,
    PRIME_TIMEOUT_S,
    ShowMode,
)
from showsim.controller import ShowController, SimClock
from showsim.constants import ENCORE_MIN_GAP_S, ENCORE_WINDOW_S, MAX_ENCORES  # noqa: E402


def _mk_cues():
    """Controller with on_cycle_locked/on_cue recorders (like the live bridge)."""
    locked, cues = [], []
    ctrl, clock, log = _mk()
    ctrl.on_cycle_locked = locked.append
    ctrl.on_cue = lambda c, phase: cues.append((c, phase))
    return ctrl, clock, log, locked, cues


def test_encore_after_mainshow_is_banter_only_same_theme() -> None:
    ctrl, clock, log, locked, cues = _mk_cues()
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.FRIENDLY)
    show = locked[-1]
    ctrl.walkway_trigger()                                   # MAIN-SHOW
    draws = len(ctrl.status()["draws"])
    clock.advance(ENCORE_MIN_GAP_S + 1)
    ctrl.walkway_trigger()                                   # same group, still here
    enc, phase = cues[-1]
    assert phase == "encore" and enc.encore
    assert (enc.theme, enc.tone) == (show.theme, show.tone)  # same theme + tone
    assert enc.session_id != show.session_id                 # own session for its params
    assert len(ctrl.status()["draws"]) == draws              # no deck card burned
    assert not any(p == "mainshow" for c, p in cues if c is enc)   # no props/music


def test_encore_min_gap_and_cap() -> None:
    ctrl, clock, log, locked, cues = _mk_cues()
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.SCARY)
    ctrl.walkway_trigger()
    clock.advance(10)
    ctrl.walkway_trigger()                                   # too soon after the show
    assert sum(p == "encore" for _, p in cues) == 0
    for _ in range(MAX_ENCORES + 3):                         # keeps lingering
        clock.advance(ENCORE_MIN_GAP_S + 1)
        ctrl.walkway_trigger()
    assert sum(p == "encore" for _, p in cues) == MAX_ENCORES
    assert any("encore cap" in m for m in log)


def test_encore_window_expires_to_a_new_group() -> None:
    ctrl, clock, log, locked, cues = _mk_cues()
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.SCARY)
    ctrl.walkway_trigger()
    draws = len(ctrl.status()["draws"])
    clock.advance(ENCORE_WINDOW_S + 1)                       # walkway clear long enough
    ctrl.walkway_trigger()
    assert sum(p == "encore" for _, p in cues) == 0
    assert any("abbreviated cycle" in m for m in log)        # a genuinely new group
    # 09-28 (Joe): still inside THEME_LOCK_S of the show -> same theme, no card spent
    assert len(ctrl.status()["draws"]) == draws
    assert any("theme LOCKED" in m for m in log)


def test_new_preshow_group_clears_encore_state() -> None:
    ctrl, clock, log, locked, cues = _mk_cues()
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.SCARY)
    ctrl.walkway_trigger()
    clock.advance(20)
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.FRIENDLY)  # next group stops at the wait spot
    clock.advance(ENCORE_MIN_GAP_S + 1)
    ctrl.walkway_trigger()                                      # their own MAIN-SHOW, not an encore
    assert cues[-2][1] == "mainshow"
    assert sum(p == "encore" for _, p in cues) == 0


def _mk() -> tuple[ShowController, SimClock, list[str]]:
    log: list[str] = []
    clock = SimClock()
    ctrl = ShowController(emit=log.append, clock=clock, rng_seed=7)
    return ctrl, clock, log


def test_prime_is_silent_and_burns_no_card() -> None:
    ctrl, clock, log = _mk()
    ctrl.prime_trigger()
    assert ctrl.status()["state"] == "PRIMED"
    assert ctrl.status()["draws"] == []                       # no deck draw
    assert not any("narrator handoff" in m for m in log)      # no audio
    clock.advance(PRIME_TIMEOUT_S + 1)
    ctrl.tick()
    assert ctrl.status()["state"] == "IDLE"
    assert ctrl.status()["draws"] == []                       # still none


def test_waiting_zone_locks_and_narrates() -> None:
    ctrl, clock, log = _mk()
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.SCARY)
    st = ctrl.status()
    assert st["state"] == "HOLD_ARMED"
    assert st["cycle"]["tone"] == "SCARY"
    assert len(st["draws"]) == 1
    assert any("preshow" in m for m in log)


def test_manual_override_beats_ai() -> None:
    ctrl, clock, _ = _mk()
    ctrl.set_manual_tone(ShowMode.SCARY)
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.FRIENDLY)
    assert ctrl.status()["cycle"]["tone"] == "SCARY"


def test_uncertain_ai_uses_fallback_default() -> None:
    ctrl, _, _ = _mk()
    ctrl.waiting_zone_trigger(ai_tone_hint=None)              # uncertain / failed call
    assert ctrl.status()["cycle"]["tone"] == "SCARY"          # FALLBACK_TONE


def test_hold_loop_caps_then_goes_quiet() -> None:
    ctrl, clock, log = _mk()
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.SCARY)
    for i in range(HOLD_MAX_LOOPS):
        clock.advance(HOLD_INTERVAL_S)
        ctrl.tick()
        assert ctrl.status()["cycle"]["hold_count"] == i + 1
    hold_lines = [m for m in log if "/ hold" in m and "narrator handoff" in m]
    assert len(hold_lines) == HOLD_MAX_LOOPS
    # one more tick past the cap: no further taunt, state goes quiet
    clock.advance(HOLD_INTERVAL_S)
    ctrl.tick()
    assert ctrl.status()["cycle"]["hold_count"] == HOLD_MAX_LOOPS
    assert ctrl.status()["state"] == "HOLD_SPENT"


def test_hold_loop_resets_when_visitor_leaves() -> None:
    ctrl, clock, _ = _mk()
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.SCARY)
    ctrl.set_presence(False)
    clock.advance(HOLD_INTERVAL_S)
    ctrl.tick()
    assert ctrl.status()["state"] == "IDLE"
    assert ctrl.status()["cycle"] is None


def test_walkway_fires_out_of_hold_with_no_cooldown() -> None:
    ctrl, clock, log = _mk()
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.SCARY)
    clock.advance(HOLD_INTERVAL_S)
    ctrl.tick()                                               # one taunt
    ctrl.walkway_trigger()
    assert any("mainshow" in m for m in log)
    assert any("SHOW_ACTIVE" in m for m in log)
    assert ctrl.status()["state"] == "IDLE"                   # cycle cleared


def test_waiting_zone_reenter_never_reruns_preshow() -> None:
    ctrl, clock, log = _mk()
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.SCARY)
    n_preshow = sum("cycle 1 START" in m for m in log)
    clock.advance(3)
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.FRIENDLY)   # tone hint ignored too
    assert any("presence re-confirmed" in m for m in log)
    assert sum("cycle 1 START" in m for m in log) == n_preshow  # not re-run
    assert ctrl.status()["cycle"]["number"] == 1
    assert ctrl.status()["cycle"]["tone"] == "SCARY"            # locked tone unchanged


def test_walkway_with_no_cycle_runs_abbreviated() -> None:
    ctrl, _, log = _mk()
    ctrl.walkway_trigger()
    assert any("abbreviated cycle" in m for m in log)
    assert len(ctrl.status()["draws"]) == 1


def test_abbreviated_walkway_debounces_rapid_retriggers() -> None:
    """2026-09-17 field bug: Frigate track-ID churn on one lingering visitor
    re-fired walkway_trigger() 4x in ~24s with no active cycle each time,
    each one publishing a real MQTT cue -- but only 2 of 4 ever reached
    Scarecrow because its OUTRO playback (10-15s, fully blocking) can't
    service a new cue that fast. A repeat within ABBREV_WALKWAY_COOLDOWN_S
    must be a no-op re-confirm, not a fresh draw+cue."""
    ctrl, clock, log = _mk()
    ctrl.walkway_trigger()
    assert len(ctrl.status()["draws"]) == 1
    fired_at = ctrl._last_abbrev_walkway_at   # the actual reference point the cooldown checks against

    # land clearly inside the cooldown window relative to fired_at
    clock.advance(max(0.0, fired_at + ABBREV_WALKWAY_COOLDOWN_S - 5 - clock()))
    assert clock() - fired_at < ABBREV_WALKWAY_COOLDOWN_S
    ctrl.walkway_trigger()
    assert len(ctrl.status()["draws"]) == 1, "retrigger inside the cooldown must not draw again"
    assert any("re-confirm only" in m for m in log)

    # now land clearly past it. 2026-09-24 (Joe): the SAME lingering group
    # now gets a banter-only ENCORE (same theme, no deck draw), not a fresh
    # abbreviated cycle with a new theme.
    clock.advance(ABBREV_WALKWAY_COOLDOWN_S)
    assert clock() - fired_at >= ABBREV_WALKWAY_COOLDOWN_S
    ctrl.walkway_trigger()
    assert len(ctrl.status()["draws"]) == 1, "a lingering group must not burn a new deck card"
    assert any("ENCORE 1/" in m for m in log)


def test_idle_timeout_resets_open_cycle() -> None:
    ctrl, clock, _ = _mk()
    ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.SCARY)
    # exhaust the taunts so the hold loop stops re-arming last_activity
    for _ in range(HOLD_MAX_LOOPS + 1):
        clock.advance(HOLD_INTERVAL_S)
        ctrl.tick()
    assert ctrl.status()["state"] == "HOLD_SPENT"
    clock.advance(CYCLE_IDLE_TIMEOUT_S)
    ctrl.tick()
    assert ctrl.status()["state"] == "IDLE"


def test_deck_does_not_repeat_across_cycles() -> None:
    ctrl, clock, _ = _mk()
    themes = []
    for _ in range(6):
        ctrl.waiting_zone_trigger(ai_tone_hint=ShowMode.SCARY)
        themes.append(ctrl.status()["cycle"]["theme"])
        ctrl.walkway_trigger()
        clock.advance(THEME_LOCK_S + 5)   # 09-28: inside the lock the theme is reused on purpose
    for a, b in zip(themes, themes[1:]):
        assert a != b, f"back-to-back theme repeat: {themes}"


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"PASS  {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL  {t.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)
