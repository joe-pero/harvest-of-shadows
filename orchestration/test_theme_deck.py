#!/usr/bin/env python3
"""
Tests for the ported Fisher-Yates anti-repeat theme deck.

Run: python3 test_theme_deck.py   (plain stdlib, no pytest needed)
"""
from __future__ import annotations

import random
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from showsim.constants import ShowTheme
from showsim.theme_deck import ThemeDeck


def test_no_back_to_back_repeats() -> None:
    """The spec's core requirement: a theme never immediately repeats."""
    for seed in range(200):
        deck = ThemeDeck(random.Random(seed))
        draws = [deck.next() for _ in range(300)]
        repeats = [(i, a) for i, (a, b) in enumerate(zip(draws, draws[1:])) if a == b]
        assert not repeats, f"seed {seed}: back-to-back repeat at {repeats[:3]}"


def test_each_deck_is_a_full_permutation() -> None:
    """Every group of 3 consecutive draws is exactly {CORN, ROTLD, MUTANT_PUMPKINS}."""
    for seed in range(200):
        deck = ThemeDeck(random.Random(seed))
        draws = [int(deck.next()) for _ in range(3 * 400)]
        for start in range(0, len(draws), 3):
            triplet = sorted(draws[start:start + 3])
            assert triplet == [0, 1, 2], f"seed {seed}: deck at {start} not a permutation: {triplet}"


def test_distribution_is_even() -> None:
    """Over many draws each theme appears ~1/3 of the time."""
    deck = ThemeDeck(random.Random(1234))
    counts = Counter(deck.next() for _ in range(30_000))
    for theme in ShowTheme:
        frac = counts[theme] / 30_000
        assert 0.31 < frac < 0.36, f"{theme.name}: {frac:.3f} (expected ~0.333)"


def test_boundary_repeat_is_actively_prevented() -> None:
    """
    Force the exact case the anti-repeat guard exists for: the last card of one
    deck equal to the first card of the next. With the guard, still no repeat.
    Without a working guard this test would catch it across enough seeds.
    """
    hits_boundary = 0
    for seed in range(2000):
        deck = ThemeDeck(random.Random(seed))
        prev = None
        for _ in range(60):
            cur = deck.next()
            if prev is not None:
                assert cur != prev
                st = deck.peek_state()
                if st["pos"] == 1:            # we just handed out card 0 of a fresh deck
                    hits_boundary += 1
            prev = cur
    assert hits_boundary > 100, f"only exercised the deck boundary {hits_boundary}x — weak test"


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
