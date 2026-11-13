#!/usr/bin/env python3
"""Offline tests for livebridge/slot_fit.py -- the fixed rules that fit the
live clothing insert to each template's sentence. No network, no audio.

    python3 orchestration/test_slot_fit.py
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from livebridge.slot_fit import (ANY, BARE_SINGULAR, NEEDS_ARTICLE, SINGULAR_VERB,  # noqa: E402
                                 fit, insert_form, slot_class)

LINES = HERE / "voice" / "lines"

SLOT_CASES = [
    ("That's a wonderful", "! The old candle...", BARE_SINGULAR),
    ("That", "you're wearing-- that whole row...", BARE_SINGULAR),
    ("That's", "you've got there.", NEEDS_ARTICLE),
    ("Careful now—", "is exactly the kind of thing", SINGULAR_VERB),
    ("I don't blame it.", "is the best thing I've seen", SINGULAR_VERB),
    ("Something crossed onto my dirt wearing", ", and the gate...", ANY),
    ("the moment", "showed up.", ANY),
    ("", "lighting up that pile of husks", ANY),
]

FORM_CASES = [
    ("a dinosaur onesie", False, ["a dinosaur onesie"]),
    ("a bright yellow raincoat and a pumpkin candy bucket", True,
     ["a bright yellow raincoat", "a pumpkin candy bucket"]),
    ("witch hats and black capes, one of them dragging a plastic pitchfork", True,
     ["witch hats", "black capes"]),
    ("a Spider-Man suit with a glowing mask", False, ["a Spider-Man suit"]),
    ("ripped jeans", True, ["ripped jeans"]),
    ("a princess dress", False, ["a princess dress"]),
]

# (insert, slot class, a_text, expected fitted text or None)
FIT_CASES = [
    ("witch hats and black capes", ANY, "", "witch hats and black capes"),
    ("witch hats and black capes", BARE_SINGULAR, "That's a wonderful", None),
    ("witch hats and black capes", SINGULAR_VERB, "", None),
    ("witch hats and black capes", NEEDS_ARTICLE, "", "witch hats and black capes"),
    ("a bright yellow raincoat and a pumpkin candy bucket", BARE_SINGULAR,
     "That's a wonderful", "bright yellow raincoat"),
    ("a bright yellow raincoat and a pumpkin candy bucket", SINGULAR_VERB, "",
     "a bright yellow raincoat"),
    ("a Spider-Man suit with a glowing mask", BARE_SINGULAR, "That's a wonderful",
     "Spider-Man suit with a glowing mask"),
    ("dinosaur onesie", NEEDS_ARTICLE, "", "a dinosaur onesie"),
    ("orange cape", NEEDS_ARTICLE, "", "an orange cape"),
    ("orange cape", BARE_SINGULAR, "That's a", None),        # "a orange" can't be fixed
    ("orange cape", BARE_SINGULAR, "That's an", "orange cape"),
    ("a princess dress", SINGULAR_VERB, "", "a princess dress"),
    ("ripped jeans", SINGULAR_VERB, "", None),
    ("", ANY, "", None),
]

REAL_INSERTS = [
    "witch hats and black capes, one of them dragging a plastic pitchfork",
    "a bright yellow raincoat and a pumpkin candy bucket",
    "matching pirate coats and foam swords",
    "a dinosaur onesie",
    "dark jackets and jeans",
    "a Spider-Man suit with a glowing mask",
]


def _templates():
    for f in sorted(LINES.glob("*/middle_templates/*.txt")):
        for line in f.read_text().splitlines():
            if line.startswith("#") or "{CLOTHING}" not in line:
                continue
            a, _, b = line.strip().partition("{CLOTHING}")
            yield f, a.strip(), b.strip()


def run() -> int:
    fails = 0

    def check(label, got, want):
        nonlocal fails
        ok = got == want
        fails += 0 if ok else 1
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}" + ("" if ok else f"  <- got {got!r} want {want!r}"))

    print("== slot_class() ==")
    for a, b, want in SLOT_CASES:
        check(f"{a[-22:]!r} [C] {b[:18]!r}", slot_class(a, b), want)

    print("\n== insert_form() ==")
    for ins, plural, items in FORM_CASES:
        f = insert_form(ins)
        check(f"{ins[:50]!r}", (f.plural, f.items), (plural, items))

    print("\n== fit() ==")
    for ins, cls, a, want in FIT_CASES:
        check(f"{ins[:34]!r} in {cls} ({a!r})", fit(ins, cls, a), want)

    print("\n== coverage over every real template ==")
    temps = list(_templates())
    classes = {}
    for _, a, b in temps:
        classes[slot_class(a, b)] = classes.get(slot_class(a, b), 0) + 1
    print(f"  {len(temps)} templates by slot class: {classes}")
    for ins in REAL_INSERTS:
        n = sum(1 for _, a, b in temps if fit(ins, slot_class(a, b), a) is not None)
        ok = n >= len(temps) * 0.8                 # every insert must still have plenty of homes
        fails += 0 if ok else 1
        print(f"  [{'PASS' if ok else 'FAIL'}] fits {n:4d}/{len(temps)}  {ins!r}")

    print(f"\n{'ALL PASS' if fails == 0 else f'{fails} FAILURE(S)'}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(run())
