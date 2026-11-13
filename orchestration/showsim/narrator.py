"""
Narrator handoff — the two A1S nodes, stand-in output.

Spec v2 "Resolved decisions — round 2":
  * Scarecrow (the villain) speaks FIRST.
  * Skeleton (an ancient protector / friend who has crossed over) FOLLOWS UP
    on the scarecrow's side.
  * "Cohabitation, not overlap" — they never talk over each other.

This module proves the *ordering*, not dialogue quality. Lines are canned
templates keyed by (theme, tone, phase). In the real system these become a
vision+TTS generation for pre-show and a short continuation for the main show;
the phase-2 line is a "follow-up" ("too late", "I warned you"), not a fresh
generation (spec v1 §main-show).

Phases: "preshow" | "hold" | "mainshow". "hold" is the taunt-the-waiter loop
(docs/show-trigger-geometry.md §2c) — in the real system a pre-rendered
`hold/<theme>_<tone>` pool, here one canned pair per bucket.
"""
from __future__ import annotations

import time
from typing import Callable

from .constants import NARRATOR_LINE_SIM_S, ShowMode, ShowTheme, THEME_LABEL

# (theme, tone, phase) -> (scarecrow_line, skeleton_line)
# phase: "preshow" | "mainshow".  Templated + deliberately terse.
_LINES: dict[tuple[ShowTheme, ShowMode, str], tuple[str, str]] = {
    (ShowTheme.CORN, ShowMode.SCARY, "preshow"): (
        "The rows counted you before you reached the gate. Keep walking.",
        "I have walked these fields when they were only soil. Mind where you step.",
    ),
    (ShowTheme.CORN, ShowMode.SCARY, "hold"): (
        "Still there? The rows counted you twice now. They don't usually get a second look.",
        "You're welcome to wait — but the soil takes a waiting man for a planted one.",
    ),
    (ShowTheme.CORN, ShowMode.SCARY, "mainshow"): (
        "Too late. The field has closed behind you.",
        "I warned you at the gate. Stay near me now.",
    ),
    (ShowTheme.CORN, ShowMode.FRIENDLY, "preshow"): (
        "The corn is restless tonight, little one — but it likes visitors.",
        "Come ahead. Nothing in this field is unkind to the small and brave.",
    ),
    (ShowTheme.CORN, ShowMode.FRIENDLY, "hold"): (
        "Gone shy on me? The stalks lean in closer when you stand that still, you know.",
        "Take your time, small one. The field is patient — it has all autumn.",
    ),
    (ShowTheme.CORN, ShowMode.FRIENDLY, "mainshow"): (
        "You made it through! The rows are cheering, in their way.",
        "Well done. I never doubted you for a moment.",
    ),

    (ShowTheme.ROTLD, ShowMode.SCARY, "preshow"): (
        "They can smell you from the yard. They are already getting up.",
        "The dead here do not rest. I have held this line a long time — hurry.",
    ),
    (ShowTheme.ROTLD, ShowMode.SCARY, "hold"): (
        "Still here. They've had time to notice a straggler now, and they do love a straggler.",
        "I would not stand still so long, friend. Still things down here get mistaken for the resting kind.",
    ),
    (ShowTheme.ROTLD, ShowMode.SCARY, "mainshow"): (
        "Too late to run. They are between you and the street now.",
        "I told you they would rise. Get behind me.",
    ),
    (ShowTheme.ROTLD, ShowMode.FRIENDLY, "preshow"): (
        "Some of the neighbours are a little... up-and-about tonight. Wave hello.",
        "They are friendlier than they look, small one. I will walk you in.",
    ),
    (ShowTheme.ROTLD, ShowMode.FRIENDLY, "hold"): (
        "Waiting for a proper welcome? The neighbours are slow risers — give them a moment.",
        "No rush, little one. The old bones will keep the restless ones back a while yet.",
    ),
    (ShowTheme.ROTLD, ShowMode.FRIENDLY, "mainshow"): (
        "See? They only wanted to say hello. Mostly.",
        "Safe and sound. The old bones kept watch.",
    ),

    (ShowTheme.MUTANT_PUMPKINS, ShowMode.SCARY, "preshow"): (
        "The patch has been growing things it should not. They noticed you.",
        "I have seen what crawls out of those vines by moonlight. Do not linger.",
    ),
    (ShowTheme.MUTANT_PUMPKINS, ShowMode.SCARY, "hold"): (
        "Rooted to the spot? Careful — that's how the patch starts something new.",
        "The vines have counted your shoes twice. Standing still only gives them the time they want.",
    ),
    (ShowTheme.MUTANT_PUMPKINS, ShowMode.SCARY, "mainshow"): (
        "Too late. The vines have the path now.",
        "I warned you the patch was awake. Stay close.",
    ),
    (ShowTheme.MUTANT_PUMPKINS, ShowMode.FRIENDLY, "preshow"): (
        "The pumpkins got a bit big this year, little one. They are showing off.",
        "Come and see. The strange ones in the patch are gentle with children.",
    ),
    (ShowTheme.MUTANT_PUMPKINS, ShowMode.FRIENDLY, "hold"): (
        "Still deciding? The big one at the back is showing off just for you — it noticed you stop.",
        "Waiting's fine, small one. The patch is gentle. Mostly it just wants to be looked at.",
    ),
    (ShowTheme.MUTANT_PUMPKINS, ShowMode.FRIENDLY, "mainshow"): (
        "You walked right past the biggest one. Very brave.",
        "Nicely done. The patch will remember you kindly.",
    ),
}


def _line(theme: ShowTheme, tone: ShowMode, phase: str) -> tuple[str, str]:
    tone = tone if tone in (ShowMode.FRIENDLY, ShowMode.SCARY) else ShowMode.SCARY
    return _LINES[(theme, tone, phase)]


class NarratorHandoff:
    """Sequential Scarecrow -> Skeleton delivery with an overlap guard."""

    def __init__(
        self,
        emit: Callable[[str], None] = print,
        line_seconds: float = NARRATOR_LINE_SIM_S,
        sleep: Callable[[float], None] = time.sleep,
    ) -> None:
        self._emit = emit
        self._line_seconds = line_seconds
        self._sleep = sleep
        self._speaking = False   # overlap guard

    def deliver(self, theme: ShowTheme, tone: ShowMode, phase: str) -> None:
        assert not self._speaking, "narrator overlap — a line started before the previous finished"
        scarecrow, skeleton = _line(theme, tone, phase)
        label = f"{THEME_LABEL[theme]} / {ShowMode(tone).name} / {phase}"

        self._speaking = True
        self._emit(f"    ── narrator handoff ({label}) ──")
        self._emit(f"    [SCARECROW] {scarecrow}")
        self._sleep(self._line_seconds)          # scarecrow is speaking
        self._emit(f"    [SKELETON ] {skeleton}")
        self._sleep(self._line_seconds)          # skeleton is speaking
        self._speaking = False
