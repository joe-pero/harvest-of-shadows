"""A simulated show night, no hardware needed.

    cd orchestration
    python3 -m showsim.demo                 # one night, ~12 groups, instant
    python3 -m showsim.demo --groups 30 --seed 7
    python3 -m showsim.demo --verbose       # every state-machine line + narrator dialogue
    python3 -m showsim.demo --pace 0.02     # watch it play out (seconds of real time per simulated second)

It drives the real show state machine (showsim.controller.ShowController, the same class the
live service runs) with a scripted mix of visitors:

  passerby   walks past on the sidewalk and keeps going (prime, then timeout)
  committed  stops at the waiting spot, then walks up the path (pre-show -> main show)
  hesitant   stops, doesn't move; gets one or two "still standing there?" taunts, then walks up
  lingerer   walks up, watches the show, and comes back up the path for an encore
  leaver     stops for the pre-show, then leaves (the visit times out and resets)

The cue-confirmation layer is simulated too: each cue sent to the yard is "confirmed" unless
the narrator board's status link happens to be down at that moment (--dropout, default 4%),
in which case it's flagged, the way the live service flags it. Clock is simulated: a whole
night runs in well under a second unless you pass --pace.
"""
from __future__ import annotations

import argparse
import random
import time
from collections import Counter

from .constants import (ENCORE_MIN_GAP_S, HOLD_INTERVAL_S, PRIME_TIMEOUT_S, SHOW_ACTIVE_MS, THEME_LABEL,
                        ShowMode)
from .controller import ShowController, SimClock

KINDS = ["passerby", "committed", "committed", "hesitant", "lingerer", "leaver"]
START_H, START_M = 17, 30          # the simulated night starts at 17:30


class Night:
    def __init__(self, seed: int, dropout: float, verbose: bool, pace: float):
        self.rng = random.Random(seed)
        self.clock = SimClock()
        self.verbose = verbose
        self.pace = pace
        self.dropout = dropout
        self.ctl = ShowController(emit=self._emit, clock=self.clock, rng_seed=seed)
        self.ctl.on_cycle_locked = self._locked
        self.ctl.on_cue = self._cue
        self.stats = Counter()
        self.themes = Counter()
        self.tones = Counter()
        self.unconfirmed = []

    # -- plumbing ------------------------------------------------------------------------
    def _emit(self, line: str) -> None:
        if self.verbose:
            print("      " + line)

    def stamp(self) -> str:
        t = int(self.clock())
        m = START_M + t // 60
        return "%02d:%02d:%02d" % (START_H + m // 60, m % 60, t % 60)

    def say(self, msg: str) -> None:
        print(f"{self.stamp()}  {msg}")

    def wait(self, seconds: float) -> None:
        """Let simulated time pass, ticking the state machine once a second."""
        end = self.clock() + seconds
        while self.clock() < end:
            step = min(1.0, end - self.clock())
            self.clock.advance(step)
            if self.pace:
                time.sleep(step * self.pace)
            self.ctl.tick()

    def _locked(self, cycle) -> None:
        self.stats["cycles locked"] += 1
        if not cycle.encore:
            self.themes[THEME_LABEL[cycle.theme]] += 1
            self.tones[cycle.tone.name.lower()] += 1

    def _cue(self, cycle, phase: str) -> None:
        self.stats["cues sent"] += 1
        self.stats[f"cue:{phase}"] += 1
        if self.rng.random() < self.dropout:
            self.unconfirmed.append((self.stamp(), phase, cycle.number))
            self.say(f"   ! {phase} cue NOT confirmed (narrator status link down): flagged for review")

    # -- visitors --------------------------------------------------------------------------
    def visitor(self, n: int, kind: str) -> None:
        tone = self.rng.choice([ShowMode.SCARY, ShowMode.SCARY, ShowMode.FRIENDLY, None])
        read = {ShowMode.SCARY: "older group", ShowMode.FRIENDLY: "little kids", None: "AI unsure"}[tone]
        self.stats["groups"] += 1
        self.ctl.prime_trigger()
        if kind == "passerby":
            self.say(f"group {n:>2}: walks past on the sidewalk (silent warm-up, no show)")
            self.wait(PRIME_TIMEOUT_S + 2)
            self.stats["passersby"] += 1
            return

        self.wait(self.rng.uniform(4, 12))
        self.ctl.waiting_zone_trigger(ai_tone_hint=tone)
        st = self.ctl.status()["cycle"]
        self.stats["visits"] += 1
        self.say(f"group {n:>2}: stops at the waiting spot ({read}) -> pre-show, "
                 f"theme {st['theme']}, tone {st['tone'].lower()}")

        if kind == "leaver":
            self.ctl.set_presence(False)
            self.say("          leaves without walking up; the visit times out and the yard resets")
            self.wait(300 + 2)
            self.stats["left early"] += 1
            return

        if kind == "hesitant":
            taunts = self.rng.choice([1, 2])
            before = self.ctl.status()["cycle"]["hold_count"]
            self.wait(HOLD_INTERVAL_S * taunts + 6)
            got = self.ctl.status()["cycle"]["hold_count"] - before
            self.stats["hold taunts"] += got
            self.say(f"          hesitates: {got} \"still standing there?\" taunt{'s' if got != 1 else ''}")
        else:
            self.wait(self.rng.uniform(8, 25))

        self.ctl.walkway_trigger()
        self.stats["main shows"] += 1
        self.say("          walks up the path -> MAIN SHOW: narrator outro, then the whole yard on one cue")
        show_s = SHOW_ACTIVE_MS / 1000 + 2
        if kind == "lingerer":
            # still on the path partway through the show: one more trip up the walkway
            self.wait(ENCORE_MIN_GAP_S + 5)
            before = self.stats["cue:encore"]
            self.ctl.walkway_trigger()
            if self.stats["cue:encore"] > before:
                self.stats["encores"] += 1
                self.say("          still on the path -> ENCORE (same theme, banter only, no new show)")
            show_s -= ENCORE_MIN_GAP_S + 5
        self.wait(show_s)

    def run(self, groups: int) -> None:
        print(f"Harvest of Shadows: simulated night ({groups} groups)\n")
        for n in range(1, groups + 1):
            self.visitor(n, self.rng.choice(KINDS))
            self.wait(self.rng.uniform(20, 240))       # quiet time before the next group
        self.summary()

    def summary(self) -> None:
        s = self.stats
        mins = int(self.clock() // 60)
        print("\n" + "-" * 64)
        print(f"Night summary ({mins // 60} h {mins % 60:02d} min simulated)")
        rows = [
            ("Groups on the sidewalk", s["groups"]),
            ("Visits (stopped at the waiting spot)", s["visits"]),
            ("Main shows", s["main shows"]),
            ("Encores", s["encores"]),
            ("\"Still standing there?\" taunts", s["hold taunts"]),
            ("Left early (visit timed out)", s["left early"]),
            ("Walked past (no show, no cost)", s["passersby"]),
            ("Cues sent to the yard", s["cues sent"]),
            ("Cues flagged as unconfirmed", len(self.unconfirmed)),
        ]
        for k, v in rows:
            print(f"  {k:<40} {v}")
        print("  Themes: " + ", ".join(f"{k} {v}" for k, v in self.themes.most_common()))
        print("  Tones:  " + ", ".join(f"{k} {v}" for k, v in self.tones.most_common()))
        if self.unconfirmed:
            print("\n  Flagged for review (the live service logs these; see 'When the numbers looked wrong' in the README):")
            for t, phase, cyc in self.unconfirmed:
                print(f"    {t}  {phase} cue, visit {cyc}")
        print("-" * 64)


def main(argv=None) -> None:
    p = argparse.ArgumentParser(prog="python3 -m showsim.demo", description="Run a simulated show night.")
    p.add_argument("--groups", type=int, default=12)
    p.add_argument("--seed", type=int, default=31, help="same seed = same night")
    p.add_argument("--dropout", type=float, default=0.04, help="chance a cue goes unconfirmed (0-1)")
    p.add_argument("--verbose", action="store_true", help="show every state-machine line and the dialogue")
    p.add_argument("--pace", type=float, default=0.0, help="real seconds per simulated second (0 = instant)")
    a = p.parse_args(argv)
    Night(a.seed, a.dropout, a.verbose, a.pace).run(a.groups)


if __name__ == "__main__":
    main()
