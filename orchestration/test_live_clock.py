"""Regression for the 2026-09-28 live-show freeze: the controller must run on
real time in the live bridge. Patches time.monotonic so it runs instantly."""
import sys
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))
from showsim.constants import ENCORE_WINDOW_S  # noqa: E402
from showsim.controller import ShowController  # noqa: E402


class FakeMono:
    t = 1000.0

    def __call__(self):
        return self.t


def test_second_group_fires_after_window():
    fake = FakeMono()
    with mock.patch("time.monotonic", fake):
        sys.path.insert(0, str(Path(__file__).parent / "livebridge"))
        from livebridge.service import LiveClock
        cues = []
        c = ShowController(clock=LiveClock(), emit=lambda m: None,
                           on_cue=lambda cyc, ph: cues.append(ph))
        c.waiting_zone_trigger()
        c.walkway_trigger()
        assert cues.count("mainshow") == 1, cues
        fake.t += ENCORE_WINDOW_S + 60          # first group long gone
        c.tick()
        c.walkway_trigger()                     # new group straight onto the walkway
        assert cues.count("mainshow") == 2, cues   # SimClock: "same group, re-confirm only" forever


if __name__ == "__main__":
    test_second_group_fires_after_window()
    print("PASS test_live_clock")
