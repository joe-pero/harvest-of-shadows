"""
Theme deck — Fisher-Yates shuffle with deck-boundary anti-repeat.

Ported 1:1 from nextTheme() in
  ~/Documents/Halloween-Dev/PlatformIO/Master Halloween Show 2026/
  nodes/main_switch_controller/src/main.cpp   (lines ~138-162)

Behaviour, identical to the firmware:
  * A "deck" is the 3 themes in a shuffled order. Each next() hands out one
    card; when the deck is exhausted it reshuffles.
  * Fisher-Yates over indices 2..1 (inclusive), j = rand in [0, i].
  * After a reshuffle, if the new first card equals the last card handed out
    (i.e. the previous deck's final card), deck[0] and deck[1] are swapped.
    That is the ONLY anti-repeat guard — within a 3-distinct-value deck a
    back-to-back repeat is otherwise impossible, so guarding the wrap point
    is sufficient.

The firmware uses Arduino random(n) -> [0, n-1]; here that is
rng.randrange(n). Pass a seeded random.Random for deterministic tests.
"""
from __future__ import annotations

import random as _random_module

from .constants import ShowTheme

_NONE = 0xFF  # matches the firmware's s_lastTheme "no last theme" sentinel


class ThemeDeck:
    def __init__(self, rng: _random_module.Random | None = None) -> None:
        self._rng = rng or _random_module.Random()
        self._deck = [0, 1, 2]
        self._pos = 3          # exhausted on first use -> shuffle
        self._last = _NONE

    def next(self) -> ShowTheme:
        if self._pos >= 3:
            for i in (2, 1):
                j = self._rng.randrange(i + 1)      # Arduino random(i+1)
                self._deck[i], self._deck[j] = self._deck[j], self._deck[i]
            self._pos = 0
            if self._deck[0] == self._last:
                self._deck[0], self._deck[1] = self._deck[1], self._deck[0]

        self._last = self._deck[self._pos]
        self._pos += 1
        return ShowTheme(self._last)

    def peek_state(self) -> dict:
        return {
            "deck": list(self._deck),
            "pos": self._pos,
            "last": None if self._last == _NONE else ShowTheme(self._last),
        }
