#!/usr/bin/env python3
"""The simulated-night demo must agree with the real state machine.

Run: python3 test_demo.py   (plain stdlib)
"""
import contextlib
import io
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from showsim.demo import Night  # noqa: E402


def run(seed, groups=25):
    n = Night(seed=seed, dropout=0.05, verbose=False, pace=0)
    with contextlib.redirect_stdout(io.StringIO()) as out:
        n.run(groups)
    return n, out.getvalue()


def main():
    for seed in range(1, 21):
        n, text = run(seed)
        s = n.stats
        assert s["groups"] == 25, seed
        assert s["visits"] == s["main shows"] + s["left early"], (seed, dict(s))
        assert s["groups"] == s["visits"] + s["passersby"], (seed, dict(s))
        assert sum(n.themes.values()) == s["visits"], (seed, dict(n.themes), dict(s))
        assert s["cycles locked"] == s["visits"] + s["encores"], (seed, dict(s))
        assert "abbreviated" not in text, f"seed {seed}: a scripted visitor fell outside the encore window"
        assert s["cue:mainshow"] == s["main shows"], (seed, dict(s))
    a, ta = run(7)
    b, tb = run(7)
    assert ta == tb, "same seed must give the same night"
    print("PASS test_demo (20 seeds x 25 groups, deterministic)")


if __name__ == "__main__":
    main()
