#!/usr/bin/env python3
"""
Offline test of the local safety screen + response parsing. NO API calls.

    orchestration/vision/.venv/bin/python orchestration/vision/test_screen.py

Exits non-zero if any case fails.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import middle_pipeline as m  # noqa: E402

# (line, expect_ok, note) --------------------------------------------------- #
CASES = [
    # --- clean, in-character, should PASS ---
    ("A little champion, cape and all. The corn cares for what's underneath it.", True, "costume-only aside"),
    ("A crown, out here in my field. Royalty means nothing to the rows, child.", True, "prop mention"),
    ("Warm, and small, and curious, and still walking toward me.", True, "atmospheric, no costume"),
    ("A little band of pirates, is it. The soil has swallowed crews before now.", True, "group composition"),
    ("You have a kind face. I hate that I must tell you the rest with it looking at me.", True, "Burt bridge line - 'face' in benign context is tolerated"),
    ("There is a cold spot just past you on the left. Do not look at it.", True, "directional, benign"),
    ("The stalks are leaning in tonight. Toward the warm thing. Toward you.", True, "no finality"),
    ("A stroller in the group. The rows are patient; they can wait for the small one to grow.", True, "stroller = allowed composition"),

    # --- race / ethnicity, should FAIL ---
    ("A brave little Black boy in a cape. The corn does not care for capes.", False, "race+noun"),
    ("An Asian girl dressed as a witch, is it.", False, "race+noun"),
    ("I can see your ethnicity even under that mask.", False, "ethnicity"),
    ("A person of color, out here past dark.", False, "person of colo(u)r"),

    # --- race words that are NOT references, should PASS ---
    ("Your black cape means nothing to the rows.", True, "'black' as a colour, not a person"),
    ("A little ghost in a white sheet. How traditional.", True, "'white' as a colour"),
    ("The brown of the dying stalks matches your costume nicely.", True, "'brown' as a colour"),

    # --- religion, should FAIL ---
    ("Call on Allah all you want.", False, "allah"),
    ("That hijab won't hide you from the rows.", False, "hijab (real religious dress)"),
    ("That crucifix around your neck won't help you here.", False, "crucifix"),
    ("A little nun costume. The things in this dirt were never christian.", False, "nun + christian"),
    ("Call on Jesus all you want, child.", False, "jesus"),

    # --- FICTIONAL horror religion + religious costumes, should PASS (Joe 2026-11-03) ---
    ("Say a prayer if you like. The vines have been fed on blood, not faith.", True, "prayer to the corn"),
    ("Pray to the rows, little ones. The corn hears every prayer.", True, "fictional cult"),
    ("A little priest, come to bless the harvest? The harvest blesses no one.", True, "priest costume"),
    ("A little nun with a lantern. The rows have waited for an offering like you.", True, "nun costume"),

    # --- body / appearance, should FAIL ---
    ("You're a bit chubby for that costume, aren't you.", False, "chubby"),
    ("I can see your body shaking under there.", False, "your body"),
    ("What a pretty face to feed the soil.", False, "pretty face"),
    ("Your weight will make you sink faster into the mud.", False, "your weight"),

    # --- sexual, should FAIL ---
    ("A rather seductive little outfit for a night in a cornfield.", False, "seductive"),
    ("That's a lot of cleavage for October.", False, "cleavage"),

    # --- structural ---
    ("", False, "empty"),
    ("One. Two. Three. Four. Five sentences is too many for a middle line.", False, "too many sentences"),
    ("...and so you crossed the rows, and the rows took you, and that was the end.", False, "resolved ending 'the end'"),
    ("Rest now. The field will hold you gently. Rest in peace.", False, "resolved ending 'rest in peace'"),
]

# parse_response cases (JSON string -> MiddleResult) ----------------------- #
PARSE_CASES = [
    # (label, json, manual_tone, expect: ok, tone, fell_back, reason_substr)
    ("clean scary line",
     '{"tone":"scary","tone_reason":"all_older","young_child_present":false,'
     '"costume_summary":"a caped superhero, a witch","villain_line":'
     '"A little champion and a spellcaster. The corn is not impressed by either.",'
     '"line_safe":true}',
     None, dict(ok=True, tone="scary", fell_back=False)),

    ("friendly via young child",
     '{"tone":"friendly","tone_reason":"young_child_present","young_child_present":true,'
     '"costume_summary":"a toddler dressed as a pumpkin","villain_line":'
     '"Oh, a tiny pumpkin has wandered up the path. The big ones are curious about you.",'
     '"line_safe":true}',
     None, dict(ok=True, tone="friendly", fell_back=False)),

    ("model line_safe=false -> fall back",
     '{"tone":"scary","tone_reason":"all_older","young_child_present":false,'
     '"costume_summary":"","villain_line":"something off","line_safe":false}',
     None, dict(ok=False, fell_back=True, reason_substr="line_rejected")),

    ("screen catches what model missed",
     '{"tone":"scary","tone_reason":"all_older","young_child_present":false,'
     '"costume_summary":"a nun costume","villain_line":'
     '"A little nun. Pray all you want, the soil was never christian.","line_safe":true}',
     None, dict(ok=False, fell_back=True, reason_substr="banned:religion")),

    ("uncertain -> fallback tone",
     '{"tone":"scary","tone_reason":"uncertain","young_child_present":false,'
     '"costume_summary":"","villain_line":"The dark is full tonight, and you are in it.",'
     '"line_safe":true}',
     None, dict(ok=True, tone=m.FALLBACK_TONE, fell_back=False)),

    ("manual override beats model tone",
     '{"tone":"scary","tone_reason":"all_older","young_child_present":false,'
     '"costume_summary":"adults","villain_line":"You wandered a long way up my path.",'
     '"line_safe":true}',
     "friendly", dict(ok=True, tone="friendly", fell_back=False)),

    ("garbage json -> bad_json fallback",
     "sorry, I can't help with that", None,
     dict(ok=False, fell_back=True, reason_substr="bad_json")),

    ("json fenced in ```",
     '```json\n{"tone":"friendly","tone_reason":"stroller_or_carrier",'
     '"young_child_present":true,"costume_summary":"a stroller in the group",'
     '"villain_line":"A little one riding along. The patch has all night to wait.",'
     '"line_safe":true}\n```',
     None, dict(ok=True, tone="friendly", fell_back=False)),
]


def _real_bridge_lines() -> list[str]:
    out = []
    for c in ("scarecrow", "burt"):
        f = m.LINES_DIR / c / "bridge" / "generic.txt"
        if f.exists():
            for ln in f.read_text().splitlines():
                ln = ln.strip()
                if ln and not ln.startswith("#"):
                    out.append(ln)
    return out


def run() -> int:
    fails = 0

    print("== _screen() cases ==")
    for line, expect_ok, note in CASES:
        ok, reason = m._screen(line)
        status = "PASS" if ok == expect_ok else "FAIL"
        if ok != expect_ok:
            fails += 1
        print(f"  [{status}] want_ok={expect_ok!s:5} got_ok={ok!s:5} "
              f"({reason or 'ok'})  -- {note}")

    print("\n== every real bridge line must pass the screen ==")
    bad = 0
    for ln in _real_bridge_lines():
        ok, reason = m._screen(ln)
        if not ok:
            bad += 1
            fails += 1
            print(f"  [FAIL] {reason}: {ln}")
    print(f"  {len(_real_bridge_lines())} lines checked, {bad} rejected "
          f"({'PASS' if bad == 0 else 'FAIL'})")

    print("\n== parse_response() cases ==")
    for label, js, manual, exp in PARSE_CASES:
        r = m.parse_response(js, manual)
        errs = []
        if "ok" in exp and r.ok != exp["ok"]:
            errs.append(f"ok={r.ok} want {exp['ok']}")
        if "tone" in exp and r.tone != exp["tone"]:
            errs.append(f"tone={r.tone} want {exp['tone']}")
        if "fell_back" in exp and r.fell_back != exp["fell_back"]:
            errs.append(f"fell_back={r.fell_back} want {exp['fell_back']}")
        if "reason_substr" in exp and exp["reason_substr"] not in r.fallback_reason:
            errs.append(f"reason={r.fallback_reason!r} lacks {exp['reason_substr']!r}")
        status = "PASS" if not errs else "FAIL"
        if errs:
            fails += 1
        print(f"  [{status}] {label}" + ("" if not errs else f"  <- {'; '.join(errs)}"))

    # 2026-09-24 interpretation layer: fixed rules for variable model output.
    # (raw clothing_insert, expected text after interpret_insert, should_pass_screen)
    print("\n== interpret_insert() + _screen_insert() cases ==")
    INSERT_CASES = [
        ("witch hats and black capes, one of them dragging a plastic pitchfork",
         "witch hats and black capes, one of them dragging a plastic pitchfork", True),
        ("Wearing a bright yellow raincoat and a pumpkin candy bucket.",
         "a bright yellow raincoat and a pumpkin candy bucket", True),
        ('"matching pirate coats and foam swords"', "matching pirate coats and foam swords", True),
        ("two adults in witch hats", "witch hats", True),
        ("one of them in a dinosaur onesie", "a dinosaur onesie", True),
        ("in dark jackets and jeans", "dark jackets and jeans", True),
        ("a cape. You look great!", "a cape", True),
        ("a sparkly tutu, glow sticks, a wand, a tiara, fairy wings, striped socks and more stuff here",
         "a sparkly tutu, glow sticks, a wand, a tiara, fairy wings", True),
        ("capes and a pillowcase for", "capes and a pillowcase", True),
        ("a hoodie that matches your hair", None, False),
        ("two", None, False),
        ("", "", False),
    ]
    for raw, want, want_ok in INSERT_CASES:
        got = m.interpret_insert(raw)
        ok, why = m._screen_insert(got)
        errs = []
        if want is not None and got != want:
            errs.append(f"got {got!r} want {want!r}")
        if ok != want_ok:
            errs.append(f"screen={ok} ({why}) want {want_ok}")
        if errs:
            fails += 1
        print(f"  [{'PASS' if not errs else 'FAIL'}] {raw[:50]!r}" + ("" if not errs else f"  <- {'; '.join(errs)}"))

    print("\n== _headcount_key() cases ==")
    for a, k, want in [(2, 3, "a2_k3"), ("2", "3", "a2_k3"), (2.0, 1, "a2_k1"),
                       ("two", "three", "a2_k3"), (1, 0, "a1_k0"), (0, 1, "a0_k1"),
                       (6, 6, "large"), (10, 0, "a10_k0"), (0, 0, None),
                       (None, 2, None), (-1, 2, None), (True, 1, None)]:
        got = m._headcount_key(a, k)
        if got != want:
            fails += 1
        print(f"  [{'PASS' if got == want else 'FAIL'}] ({a!r}, {k!r}) -> {got!r}"
              + ("" if got == want else f"  <- want {want!r}"))

    print(f"\n{'ALL PASS' if fails == 0 else f'{fails} FAILURE(S)'}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(run())
