"""
ShowController — the show-cycle state machine.

Implements `docs/show-trigger-geometry.md` (2026-09-08), which supersedes the
sidewalk / brick-walkway two-zone model of Show Logic Spec v2:

  * Camera `front_yard_east` only. Four zones:
      - PRIME    : `sidewalk_zone_1` / `sidewalk_zone_2`
      - PRE-SHOW : `waiting_zone`      (the visitor STOPS here; photo point)
      - MAIN-SHOW: `brick_walkway`     (visitor commits up the path)
  * State: IDLE → PRIMED → PRESHOW → (HOLD loop ×≤3) → MAINSHOW → ACTIVE → reset.
  * PRIME is a silent warm-up. Disposable — times out back to IDLE, burns no
    theme-deck card.
  * PRE-SHOW: one AI call (tone + MIDDLE), no latency deadline — the visitor
    is standing still. Theme+tone lock here. Scarecrow INTRO+MIDDLE → Burt.
  * HOLD: timer-driven "you're still standing there" taunt loop. Every
    HOLD_INTERVAL_S with no walkway trigger and the visitor still present: fresh
    photo (presence confirm + logged scene read only — the locked theme AND tone
    are reused for every retrigger), play a HOLD line, re-arm. Cap HOLD_MAX_LOOPS,
    then go quiet and wait out the idle timeout.
  * MAIN-SHOW: fires from ANY active state, no cooldown. OUTRO pair → theme-music
    phase (SHOW_ACTIVE_MS) → reset, lock clears, Scarecrow re-arms.
  * Tone precedence (unchanged): manual Main-Switch button ALWAYS wins; else the
    AI age read may force Friendly; else Scary.

Trigger sources here are method calls driven by the CLI. In the real layer
`prime_trigger` / `waiting_zone_trigger` / `walkway_trigger` are called from a
Frigate `frigate/events` MQTT subscriber filtering on
`(camera == front_yard_east, zone == …)`, and `deliver` / prop activation go out
over MQTT (`harvest/show/cue`) + ESP-NOW.
"""
from __future__ import annotations

import random
import time
from dataclasses import dataclass, field
from typing import Callable

from .constants import (
    ENCORE_GROUP_MAX_S,
    ENCORE_MIN_GAP_S,
    ENCORE_WINDOW_S,
    MAX_ENCORES,
    ABBREV_WALKWAY_COOLDOWN_S,
    CYCLE_IDLE_TIMEOUT_S,
    FALLBACK_TONE,
    HOLD_INTERVAL_S,
    HOLD_MAX_LOOPS,
    PRIME_TIMEOUT_S,
    SHOW_ACTIVE_MS,
    THEME_LOCK_S,
    VISIT_MAX_S,
    WAITING_MAINSHOW_AFTER_TAUNTS,
    LINGER_ENCORE_GAP_S,
    ShowMode,
    ShowTheme,
    THEME_LABEL,
)
from .narrator import NarratorHandoff
from .theme_deck import ThemeDeck


class SimClock:
    """Manually-advanced monotonic clock (seconds). Deterministic for tests."""

    def __init__(self, start: float = 0.0) -> None:
        self._t = start

    def __call__(self) -> float:
        return self._t

    def advance(self, seconds: float) -> None:
        self._t += max(0.0, seconds)


def theme_props(theme: ShowTheme) -> list[dict]:
    """Theme-linked prop actions for the main show (spec v1 §main-show).

    Only ROTLD is spelled out in the spec; CORN and MUTANT_PUMPKINS are
    placeholders for "their own equivalent prop set".

    2026-09-28: singing_pumpkins is listed for every theme (its ESP32 starts the
    card's ~3 min show on the Scarecrow's SHOW_ACTIVE, whatever the theme).
    PLACEHOLDER: log-only here like every entry; narrator-floor rules in
    docs/audio-floor-control.md.
    """
    pumpkins = {"node": "singing_pumpkins",
                "action": "show on SHOW_ACTIVE, after the narrators (PLACEHOLDER: duck under narrators)"}
    if theme is ShowTheme.ROTLD:
        return [
            {"node": "zombie_container", "action": "power_on_prop_logic"},
            {"node": "front_door_projector", "action": "select_video:rotld"},
            {"node": "spatial_audio", "action": "scene:rotld"},
            pumpkins,
        ]
    if theme is ShowTheme.CORN:
        return [
            {"node": "front_door_projector", "action": "select_video:corn"},
            {"node": "spatial_audio", "action": "scene:corn"},
            {"node": "TODO", "action": "corn prop set — not yet specified"},
            pumpkins,
        ]
    return [
        {"node": "front_door_projector", "action": "select_video:mutant_pumpkins"},
        {"node": "spatial_audio", "action": "scene:mutant_pumpkins"},
        {"node": "TODO", "action": "mutant-pumpkins prop set — not yet specified"},
        pumpkins,
    ]


@dataclass
class Cycle:
    number: int
    theme: ShowTheme
    tone: ShowMode
    started_at: float
    last_activity_at: float          # pre-show / hold / walkway — feeds idle timeout
    session_id: int                  # wire session_id for harvest/show/params + /cue
    hold_armed_at: float | None = None   # when the current HOLD_INTERVAL_S window began
    hold_count: int = 0
    ai_hint_at_lock: ShowMode | None = None
    manual_at_lock: ShowMode | None = None
    encore: bool = False             # banter-only encore for a lingering group
    banter: dict | None = None       # {"pool", "idx"} chosen by the live layer at lock
    trigger: str = "waiting_zone"    # data (09-28): waiting_zone | walkway_only | encore_walkway | encore_linger
    mainshow_source: str | None = None   # data: walkway | waiting_zone (gate-closed mode)


@dataclass
class ShowController:
    emit: Callable[[str], None] = print
    clock: Callable[[], float] = field(default_factory=SimClock)
    rng_seed: int | None = None
    # Live-layer integration seams (orchestration/livebridge/). None in the
    # simulator/tests -> zero behavior change. When set, replace the
    # `_log()`-only "would publish" lines with real MQTT publishes.
    on_cycle_locked: Callable[[Cycle], None] | None = None   # fires once, right after Cycle creation
    on_cue: Callable[[Cycle, str], None] | None = None       # fires per phase: preshow/hold/mainshow/reset
    # 2026-09-28: gate-closed mode (FREE show, AI off). Live layer sets it each
    # tick. MAIN-SHOW from the waiting zone + encores for a lingering group.
    gate_closed: bool = False

    def __post_init__(self) -> None:
        self._deck = ThemeDeck(random.Random(self.rng_seed) if self.rng_seed is not None else None)
        self._narrator = NarratorHandoff(emit=self.emit, sleep=self._sleep)
        self._cycle: Cycle | None = None
        self._manual_tone: ShowMode | None = None
        self._primed_at: float | None = None
        self._person_present: bool = True   # sim stand-in for the live Frigate zone state
        self._next_cycle_number = 1
        self._draw_log: list[ShowTheme] = []
        self._last_abbrev_walkway_at: float | None = None
        # Encore tracking (see ENCORE_* in constants.py)
        self._last_show: tuple[ShowTheme, ShowMode] | None = None
        self._last_walkway_at: float | None = None
        self._last_voice_at: float | None = None
        self._last_show_at: float | None = None
        self._last_show_theme: ShowTheme | None = None   # survives new cycles (theme lock)
        self._encores = 0

    # ── clock / narration ────────────────────────────────────────────────────
    def _sleep(self, seconds: float) -> None:
        adv = getattr(self.clock, "advance", None)
        if callable(adv):
            adv(seconds)          # SimClock: jump forward
        else:
            time.sleep(seconds)   # real clock: actually wait

    def _log(self, msg: str) -> None:
        self.emit(f"[{self.clock():7.1f}s] {msg}")

    # ── tone resolution (unchanged precedence) ───────────────────────────────
    def _resolve_tone(self, ai_hint: ShowMode | None) -> ShowMode:
        if self._manual_tone is not None:
            return self._manual_tone            # manual button ALWAYS wins
        if ai_hint is ShowMode.FRIENDLY:
            return ShowMode.FRIENDLY             # AI may only force Friendly
        if ai_hint is None:
            return FALLBACK_TONE                 # uncertain / failed call → default
        return ShowMode.SCARY                    # explicit scary read

    def set_manual_tone(self, tone: ShowMode) -> None:
        self._manual_tone = tone
        self._log(f"MANUAL OVERRIDE set: {tone.name} (this now wins over any AI age call)")

    def clear_manual_tone(self) -> None:
        self._manual_tone = None
        self._log("MANUAL OVERRIDE cleared — AI age detection decides tone again")

    def set_presence(self, present: bool) -> None:
        """Sim stand-in for 'is a person still in waiting_zone / a sidewalk
        zone', which the live controller reads from the latest Frigate state at
        each hold tick."""
        self._person_present = present
        self._log(f"presence := {present} (sim stand-in for live Frigate zone state)")

    # ── PRIME ────────────────────────────────────────────────────────────────
    def prime_trigger(self) -> None:
        """person entered `sidewalk_zone_1` / `sidewalk_zone_2`.

        Silent warm-up. No deck draw, no params, no cue, no audio.
        """
        now = self.clock()
        self._primed_at = now
        if self._cycle is not None:
            self._log("prime trigger — cycle already active; warm-up timestamp refreshed")
            return
        self._log(
            "PRIME (sidewalk zone) — warming pipeline: refresh east snapshot, wake "
            "audio bridge, pre-resolve next theme candidate (NOT drawn yet). No audio."
        )

    # ── PRE-SHOW ─────────────────────────────────────────────────────────────
    def draw_next_theme(self) -> ShowTheme:
        """Commit a real card from the deck NOW, ahead of the trigger call.

        The live layer needs the theme *before* it calls waiting_zone_trigger
        -- generate_middle()'s prompt is theme-flavored, but the AI call
        (which produces ai_tone_hint) has to happen before the trigger fires.
        Pass the result back in as `waiting_zone_trigger(theme=...)`.
        """
        return self._next_theme()

    def _next_theme(self) -> ShowTheme:
        """Deck draw, unless the audio props are still playing the last
        MAIN-SHOW's theme (THEME_LOCK_S): then reuse it, no card spent."""
        now = self.clock()
        if (self._last_show_at is not None and self._last_show_theme is not None
                and (now - self._last_show_at) < THEME_LOCK_S):
            self._log(f"theme LOCKED to {THEME_LABEL[self._last_show_theme]} "
                      f"({now - self._last_show_at:.0f}s < {THEME_LOCK_S}s since the last MAIN-SHOW, "
                      f"props still playing it) -- no deck draw")
            return self._last_show_theme
        theme = self._deck.next()
        self._draw_log.append(theme)
        return theme

    def waiting_zone_trigger(
        self,
        ai_tone_hint: ShowMode | None = None,
        session_id: int | None = None,
        theme: ShowTheme | None = None,
    ) -> None:
        """person stopped in `waiting_zone` (>= WAITING_ZONE_MIN_DWELL_S,
        enforced by the zone's loitering_time on the box).

        `ai_tone_hint` stands in for the resolved AI `tone`: FRIENDLY forces
        Friendly, SCARY is an explicit scary read, None = uncertain/failed call
        (→ FALLBACK_TONE). No latency deadline — we already have the full
        response, MIDDLE line included, before the INTRO starts.

        `session_id` lets the live layer (orchestration/livebridge/) mint the
        wire session_id itself, ahead of time, so a MIDDLE WAV can be
        pre-synthesized before this call and tied to the Cycle in
        `on_cycle_locked`. Sim/tests omit it -> a fresh id is minted here.

        `theme` lets the live layer pass in a card already drawn via
        `draw_next_theme()` (needed ahead of the AI call). Sim/tests omit
        it -> drawn here, same as before.
        """
        now = self.clock()
        self._person_present = True

        if self._cycle is None:
            if theme is None:
                theme = self._next_theme()
            tone = self._resolve_tone(ai_tone_hint)
            self._cycle = Cycle(
                number=self._next_cycle_number,
                theme=theme,
                tone=tone,
                started_at=now,
                last_activity_at=now,
                session_id=session_id if session_id is not None else random.getrandbits(32),
                ai_hint_at_lock=ai_tone_hint,
                manual_at_lock=self._manual_tone,
            )
            self._next_cycle_number += 1
            self._primed_at = None
            self._last_show = None                # a new group: no encore carry-over
            self._encores = 0
            if self.on_cycle_locked:
                self.on_cycle_locked(self._cycle)
            src = (
                "manual override" if self._manual_tone is not None
                else f"AI={ai_tone_hint.name}" if ai_tone_hint is not None
                else f"uncertain/failed → {FALLBACK_TONE.name} default"
            )
            self._log(
                f"PRE-SHOW TRIGGER (waiting_zone) — cycle {self._cycle.number} START. "
                f"snapshot → 1 AI call (tone + MIDDLE, full wait). "
                f"LOCKED theme={THEME_LABEL[theme]}  tone={tone.name} ({src})"
            )
            self._log("  publish harvest/show/params (retained) + harvest/show/cue phase=preshow")
            if self.on_cue:
                self.on_cue(self._cycle, "preshow")
            self._narrator.deliver(theme, tone, "preshow")
            self._arm_hold("pre-show narration complete")
            return

        # cycle already active: a waiting-zone re-enter NEVER re-runs the pre-show
        # (that's what the HOLD loop is for). It only re-confirms the visitor is
        # still standing there, for the next hold tick. Frigate zone-edge flicker
        # and a genuine "still waiting" re-enter collapse to the same outcome, so
        # there's no debounce to get wrong — event-stream dedup lives in the real
        # frigate/events subscriber, not here.
        c = self._cycle
        c.last_activity_at = now
        self._person_present = True
        self._log(
            "waiting-zone re-enter on an active cycle — presence re-confirmed for "
            "the next hold tick; pre-show NOT re-run (locked theme/tone unchanged)"
        )

    # ── HOLD loop ────────────────────────────────────────────────────────────
    def _arm_hold(self, why: str) -> None:
        c = self._cycle
        assert c is not None
        now = self.clock()
        c.last_activity_at = now
        c.hold_armed_at = now
        self._log(
            f"{why} → HOLD-ARMED (watching brick_walkway; hold tick in "
            f"{HOLD_INTERVAL_S}s, {c.hold_count}/{HOLD_MAX_LOOPS} taunts used)"
        )

    def _hold_tick(self) -> None:
        """Fire one HOLD taunt, or end the cycle. Called from tick() when the
        HOLD_INTERVAL_S window has elapsed with no walkway trigger."""
        c = self._cycle
        assert c is not None and c.hold_armed_at is not None

        if not self._person_present:
            self._log(
                f"hold tick — nobody in waiting_zone / sidewalk zones; visitor left. "
                f"cycle {c.number} reset (deck card {THEME_LABEL[c.theme]} stays spent)"
            )
            if self.on_cue:
                self.on_cue(c, "reset")
            self._reset_cycle()
            return

        if self.gate_closed and c.hold_count >= WAITING_MAINSHOW_AFTER_TAUNTS:
            self._log(f"GATE-CLOSED mode: still in the waiting zone after {c.hold_count} taunt(s) "
                      f"-> MAIN-SHOW from the waiting zone")
            self._mainshow(c, "waiting_zone")
            return

        if c.hold_count >= HOLD_MAX_LOOPS:
            self._log(
                f"hold tick — {HOLD_MAX_LOOPS} taunts used; going QUIET. Yard waits out "
                f"the {CYCLE_IDLE_TIMEOUT_S}s idle timeout unless they move onto the walkway."
            )
            c.hold_armed_at = None   # stop ticking; tick() idle-timeout takes it from here
            return

        c.hold_count += 1
        self._log(
            f"HOLD tick #{c.hold_count} — fresh east snapshot (presence confirm + "
            f"logged scene read only); theme AND tone stay locked → "
            f"hold/{_bucket(c.theme, c.tone)} pool"
        )
        self._log("  publish harvest/show/cue phase=hold  (same session_id, same theme+tone)")
        if self.on_cue:
            self.on_cue(c, "hold")
        self._narrator.deliver(c.theme, c.tone, "hold")
        self._arm_hold(f"hold taunt #{c.hold_count} done — re-aim")

    # ── MAIN-SHOW ────────────────────────────────────────────────────────────
    def walkway_trigger(self) -> None:
        """person entered `brick_walkway` — the final act. Fires from any
        active state (PRESHOW / HOLD-ARMED), immediately, no cooldown."""
        now = self.clock()

        if self._cycle is not None:
            self._mainshow(self._cycle, "walkway")
            return

        self._walkway_no_cycle(now)

    def _mainshow(self, c: Cycle, source: str) -> None:
        """The final act for an active cycle: from the walkway, or (gate-closed
        mode) from the waiting zone."""
        now = self.clock()
        c.hold_armed_at = None
        c.mainshow_source = source
        self._log(
            f"MAIN-SHOW TRIGGER ({'brick_walkway' if source == 'walkway' else 'waiting zone, gate closed'}) "
            f"— cycle {c.number} FINAL ACT, "
            f"firing immediately (no cooldown; {c.hold_count} hold taunts played)"
        )
        self._log("  publish harvest/show/cue phase=mainshow")
        if self.on_cue:
            self.on_cue(c, "mainshow")
        self._narrator.deliver(c.theme, c.tone, "mainshow")
        self._fire_props(c.theme)
        self._log(
            f"  CMD_SHOW_STATE(SHOW_ACTIVE) — theme-music phase, {SHOW_ACTIVE_MS // 1000}s"
        )
        self._log(f"cycle {c.number} COMPLETE — lock cleared; Scarecrow re-arms for Frigate")
        if self.on_cue:
            self.on_cue(c, "reset")
        self._mark_show(c.theme, c.tone, now)
        self._reset_cycle()

    def _walkway_no_cycle(self, now: float) -> None:
        # no active cycle, but the SAME group is still on the bricks: encore
        if (self._last_show is not None and self._last_walkway_at is not None
                and (now - self._last_walkway_at) <= ENCORE_WINDOW_S):
            if self._last_show_at is not None and (now - self._last_show_at) > ENCORE_GROUP_MAX_S:
                self._log(f"walkway trigger — {now - self._last_show_at:.0f}s since the last MAIN-SHOW "
                          f"(> {ENCORE_GROUP_MAX_S}s ceiling): treating as a NEW group")
                self._last_show = None
                self._encores = 0
            else:
                self._encore(now)
                return

        # no active cycle: reached the walkway without stopping at the wait spot
        # (walk-through, or a Frigate zone gap). Abbreviated cycle.
        if (self._last_abbrev_walkway_at is not None
                and (now - self._last_abbrev_walkway_at) < ABBREV_WALKWAY_COOLDOWN_S):
            self._log(
                f"walkway trigger with NO active cycle — re-confirm only, "
                f"{now - self._last_abbrev_walkway_at:.1f}s < "
                f"{ABBREV_WALKWAY_COOLDOWN_S}s cooldown since the last abbreviated cycle "
                f"(likely the same lingering visitor / Frigate track churn, not a new one)"
            )
            return
        self._last_abbrev_walkway_at = now
        self._log(
            "walkway trigger with NO active cycle — walk-through / zone gap. "
            "Running an abbreviated cycle (deck draw, no pre-show photo, tone = "
            "manual/default, straight to OUTRO)."
        )
        theme = self._next_theme()
        tone = self._resolve_tone(None)
        abbrev = Cycle(
            number=self._next_cycle_number,
            theme=theme,
            tone=tone,
            started_at=now,
            last_activity_at=now,
            session_id=random.getrandbits(32),
            trigger="walkway_only",
            mainshow_source="walkway",
        )
        self._log(f"  abbreviated cycle {self._next_cycle_number}: theme={THEME_LABEL[theme]} tone={tone.name}")
        self._next_cycle_number += 1
        if self.on_cycle_locked:
            self.on_cycle_locked(abbrev)
        if self.on_cue:
            self.on_cue(abbrev, "mainshow")
        self._narrator.deliver(theme, tone, "mainshow")
        self._fire_props(theme)
        if self.on_cue:
            self.on_cue(abbrev, "reset")
        self._mark_show(theme, tone, now)
        self._log("  abbreviated cycle complete")

    def _mark_show(self, theme: ShowTheme, tone: ShowMode, now: float) -> None:
        """A MAIN-SHOW just played: a lingering group may earn encores."""
        self._last_show = (theme, tone)
        self._last_walkway_at = now
        self._last_voice_at = now
        self._last_show_at = now
        self._last_show_theme = theme
        self._encores = 0

    def _encore(self, now: float, source: str = "walkway") -> None:
        """Same group lingering after MAIN-SHOW (walkway re-fire, or gate-closed
        mode: still standing in the zones): banter only."""
        self._last_walkway_at = now               # still here -> keep the window open
        if self._encores >= MAX_ENCORES:
            self._log(f"walkway re-trigger, same group — encore cap ({MAX_ENCORES}) reached, "
                      f"staying quiet until the walkway is clear {ENCORE_WINDOW_S}s")
            return
        since = now - (self._last_voice_at if self._last_voice_at is not None else now)
        if since < ENCORE_MIN_GAP_S:
            self._log(f"walkway re-trigger, same group — {since:.0f}s < {ENCORE_MIN_GAP_S}s "
                      f"since the last line, re-confirm only")
            return
        theme, tone = self._last_show
        self._encores += 1
        self._last_voice_at = now
        enc = Cycle(
            number=self._next_cycle_number,
            theme=theme,
            tone=tone,
            started_at=now,
            last_activity_at=now,
            session_id=random.getrandbits(32),
            encore=True,
            trigger=f"encore_{source}",
        )
        self._next_cycle_number += 1
        self._log(f"ENCORE {self._encores}/{MAX_ENCORES} — same group still {'on the walkway' if source == 'walkway' else 'here (lingering)'}: "
                  f"banter only ({THEME_LABEL[theme]} / {tone.name}), no props, no deck draw")
        if self.on_cycle_locked:
            self.on_cycle_locked(enc)
        if self.on_cue:
            self.on_cue(enc, "encore")

    # ── periodic ─────────────────────────────────────────────────────────────
    def tick(self) -> None:
        """Call periodically. Owns: PRIME timeout, HOLD interval firing, and the
        idle-timeout cycle reset."""
        now = self.clock()

        # Gate-closed mode: a group still standing around after the MAIN-SHOW
        # gets encores (up to MAX_ENCORES, LINGER_ENCORE_GAP_S apart).
        if (self.gate_closed and self._cycle is None and self._last_show is not None
                and self._person_present and self._encores < MAX_ENCORES
                and self._last_show_at is not None and (now - self._last_show_at) <= ENCORE_GROUP_MAX_S
                and self._last_voice_at is not None and (now - self._last_voice_at) >= LINGER_ENCORE_GAP_S):
            self._encore(now, "linger")

        # PRIME timeout — silent revert to IDLE
        if self._cycle is None and self._primed_at is not None:
            if (now - self._primed_at) >= PRIME_TIMEOUT_S:
                self._log(
                    f"PRIME timeout ({PRIME_TIMEOUT_S}s, no waiting-zone trigger) — back to IDLE"
                )
                self._primed_at = None
            return

        if self._cycle is None:
            return

        c = self._cycle

        # HOLD interval elapsed → one hold tick
        if c.hold_armed_at is not None and (now - c.hold_armed_at) >= HOLD_INTERVAL_S:
            self._hold_tick()
            return

        # Joe 2026-09-28: a visit is over after VISIT_MAX_S, whatever is going on
        if (now - c.started_at) >= VISIT_MAX_S:
            self._log(f"cycle {c.number} visit limit ({VISIT_MAX_S}s) reached -- resetting; lock cleared")
            if self.on_cue:
                self.on_cue(c, "reset")
            self._reset_cycle()
            return

        # idle timeout — cycle never reached (or finished past) the taunt loop
        if (now - c.last_activity_at) >= CYCLE_IDLE_TIMEOUT_S:
            self._log(
                f"cycle {c.number} idle timeout ({CYCLE_IDLE_TIMEOUT_S}s, no main show) "
                "— resetting; lock cleared"
            )
            if self.on_cue:
                self.on_cue(c, "reset")
            self._reset_cycle()

    # ── props / reset ────────────────────────────────────────────────────────
    def _fire_props(self, theme: ShowTheme) -> None:
        for p in theme_props(theme):
            self._log(f"  prop -> {p['node']}: {p['action']}")

    def _reset_cycle(self) -> None:
        self._cycle = None
        self._primed_at = None
        self._person_present = True

    def force_reset(self, reason: str) -> Cycle | None:
        """Live-bridge watchdog: drop the active cycle NOW. Returns the dropped
        cycle (caller publishes its reset cue) or None if idle."""
        c = self._cycle
        if c is None:
            return None
        self._log(f"FORCED RESET of cycle {c.number}: {reason}")
        self._reset_cycle()
        return c

    # ── introspection ────────────────────────────────────────────────────────
    def status(self) -> dict:
        c = self._cycle
        now = self.clock()
        if c is None:
            state = "PRIMED" if self._primed_at is not None else "IDLE"
        elif c.hold_armed_at is not None:
            state = "HOLD_ARMED"
        elif c.hold_count >= HOLD_MAX_LOOPS:
            state = "HOLD_SPENT"      # taunts used up; gone quiet, waiting out the idle timeout
        else:
            state = "PRESHOW"
        return {
            "clock_s": round(now, 1),
            "state": state,
            "manual_override": self._manual_tone.name if self._manual_tone else None,
            "person_present": self._person_present,
            "primed_for_s": None if self._primed_at is None else round(now - self._primed_at, 1),
            "cycle": None if not c else {
                "number": c.number,
                "theme": THEME_LABEL[c.theme],
                "tone": c.tone.name,
                "age_s": round(now - c.started_at, 1),
                "hold_count": c.hold_count,
                "hold_tick_in_s": None if c.hold_armed_at is None
                else max(0.0, round(HOLD_INTERVAL_S - (now - c.hold_armed_at), 1)),
                "idle_reset_in_s": max(0.0, round(CYCLE_IDLE_TIMEOUT_S - (now - c.last_activity_at), 1)),
            },
            "deck": self._deck.peek_state(),
            "draws": [t.name for t in self._draw_log],
        }


_BUCKET = {
    ShowTheme.CORN: "corn",
    ShowTheme.ROTLD: "rotld",
    ShowTheme.MUTANT_PUMPKINS: "pumpkins",
}


def _bucket(theme: ShowTheme, tone: ShowMode) -> str:
    tone = tone if tone in (ShowMode.FRIENDLY, ShowMode.SCARY) else ShowMode.SCARY
    return f"{_BUCKET[theme]}_{tone.name.lower()}"
