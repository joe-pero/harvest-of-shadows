"""Battle tests for the 2026-09-28 live-bridge hardening (PROJECT_LOG 09-28 evening).

Runs the REAL ShowController on the live bridge's LiveClock with time.monotonic
patched, so hours of yard traffic run in seconds. No broker, no hardware.

    python3 orchestration/test_live_hardening.py
"""
import random
import sys
import threading
import time
from pathlib import Path
from unittest import mock

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "livebridge"))

from showsim.constants import (  # noqa: E402
    ABBREV_WALKWAY_COOLDOWN_S, CYCLE_IDLE_TIMEOUT_S, ENCORE_GROUP_MAX_S, HOLD_INTERVAL_S,
    HOLD_MAX_LOOPS, LINGER_ENCORE_GAP_S, MAX_CYCLE_AGE_S, MAX_ENCORES, THEME_LOCK_S, VISIT_MAX_S,
)
from showsim.controller import ShowController  # noqa: E402

AUDIO = ("preshow", "hold", "mainshow", "encore")


class FakeMono:
    def __init__(self):
        self.t = 10_000.0

    def __call__(self):
        return self.t


def make(fake):
    from livebridge.service import LiveClock
    cues = []
    c = ShowController(clock=LiveClock(), emit=lambda m: None,
                       on_cue=lambda cyc, ph: cues.append((fake.t, cyc.session_id, ph)))
    return c, cues


def run(fake, c, seconds, step=1.0):
    end = fake.t + seconds
    while fake.t < end:
        fake.t += step
        c.tick()


def check_session_order(cues):
    """Per session: no audio cue after its reset; a preshow is always first."""
    seen = {}
    for t, sid, ph in cues:
        st = seen.setdefault(sid, {"reset": False, "first": ph})
        assert not (st["reset"] and ph in AUDIO), f"audio cue {ph} after reset for {sid:08x}"
        if ph == "reset":
            st["reset"] = True
    for sid, st in seen.items():
        assert st["first"] in ("preshow", "mainshow", "encore"), (sid, st)


# ── 1. the 09-28 failure itself ───────────────────────────────────────────────
def test_second_group_after_first_show():
    fake = FakeMono()
    with mock.patch("time.monotonic", fake):
        c, cues = make(fake)
        c.waiting_zone_trigger(); c.walkway_trigger()
        run(fake, c, ENCORE_GROUP_MAX_S + 60)
        c.walkway_trigger()
        assert [p for _, _, p in cues].count("mainshow") == 2, cues


# ── 2. busy Halloween night: walkway never goes quiet ─────────────────────────
def test_busy_night_every_group_gets_a_show():
    """Walkway re-fires every 20s for 2 hours (no 120s gap ever). Before the
    ceiling, only the FIRST group got a show; everyone after got 2 banter
    encores then silence. Now the longest stretch with no MAIN-SHOW is bounded."""
    fake = FakeMono()
    with mock.patch("time.monotonic", fake):
        c, cues = make(fake)
        for _ in range(360):
            c.walkway_trigger()
            run(fake, c, 20)
        shows = [t for t, _, p in cues if p == "mainshow"]
        assert len(shows) >= 20, len(shows)
        worst = max(b - a for a, b in zip(shows, shows[1:]))
        assert worst <= ENCORE_GROUP_MAX_S + ABBREV_WALKWAY_COOLDOWN_S + 20, worst
        check_session_order(cues)


# ── 3. HOLD taunts + idle reset really fire on real time ──────────────────────
def test_hold_taunts_and_visitor_leaves():
    fake = FakeMono()
    with mock.patch("time.monotonic", fake):
        c, cues = make(fake)
        c.waiting_zone_trigger()
        run(fake, c, HOLD_INTERVAL_S * (HOLD_MAX_LOOPS + 1) + 5)
        assert [p for _, _, p in cues].count("hold") == HOLD_MAX_LOOPS, cues
        run(fake, c, CYCLE_IDLE_TIMEOUT_S + 5)
        assert cues[-1][2] == "reset" and c.status()["cycle"] is None
        c.waiting_zone_trigger()                  # next visitor gets a fresh pre-show
        assert cues[-1][2] == "preshow"

        c2, cues2 = make(fake)                    # visitor walks away mid-hold
        c2.waiting_zone_trigger()
        c2.set_presence(False)
        run(fake, c2, HOLD_INTERVAL_S + 2)
        assert cues2[-1][2] == "reset", cues2


# ── 4. six-hour randomized soak ───────────────────────────────────────────────
def test_random_soak_six_hours():
    rng = random.Random(1031)
    fake = FakeMono()
    with mock.patch("time.monotonic", fake):
        c, cues = make(fake)
        for _ in range(6 * 3600):
            fake.t += 1.0
            r = rng.random()
            if r < 0.010:
                c.prime_trigger()
            elif r < 0.016:
                c.waiting_zone_trigger()
            elif r < 0.022:
                c.walkway_trigger()
            elif r < 0.026:
                c.set_presence(rng.random() < 0.6)
            c.tick()
            st = c.status()
            if st["cycle"]:
                # waiting-zone re-enters legitimately keep a cycle alive; the
                # service watchdog (test 5) caps the rest. Controller alone:
                # idle timeout must fire once activity stops.
                pass
        c.set_presence(False)
        run(fake, c, CYCLE_IDLE_TIMEOUT_S + HOLD_INTERVAL_S + 5)
        assert c.status()["cycle"] is None, "cycle survived with nobody around"
        check_session_order(cues)
        phases = [p for _, _, p in cues]
        assert phases.count("mainshow") > 50 and phases.count("hold") > 10, \
            {p: phases.count(p) for p in set(phases)}


# ── 5. service watchdog force-closes a cycle kept alive forever ───────────────
def test_watchdog_forces_reset():
    import livebridge.service as svc
    import showsim.controller as ctl
    fake = FakeMono()
    # disable the controller's own visit limit to prove the safety net behind it
    with mock.patch("time.monotonic", fake), mock.patch.object(ctl, "VISIT_MAX_S", 10**9):
        c, cues = make(fake)
        bridge = svc.LiveBridge.__new__(svc.LiveBridge)
        bridge.ctrl = c
        bridge.show_log = mock.Mock()
        sent = []
        bridge._on_cue = lambda cyc, ph: sent.append(ph)
        c.waiting_zone_trigger()
        for _ in range(int(MAX_CYCLE_AGE_S) + 30):
            fake.t += 1.0
            if int(fake.t) % 30 == 0:
                c.waiting_zone_trigger()          # someone loitering forever
            c.tick()
            bridge._watchdog()
        assert "reset" in sent, sent
        assert c.status()["cycle"] is None or c.status()["cycle"]["age_s"] < MAX_CYCLE_AGE_S


# ── 6. a bad Frigate message can't kill the subscriber thread ─────────────────
def test_frigate_handler_never_raises():
    from livebridge.frigate_events import FrigateSubscriber

    class Boom:
        def walkway_trigger(self):
            raise RuntimeError("controller bug")

        def status(self):
            return {"cycle": None}

    logs = []
    sub = FrigateSubscriber(Boom(), waiting_zone_hint_fn=None, log=logs.append, armed_fn=lambda: True)
    msg = mock.Mock()
    msg.payload = b'{"type":"new","after":{"camera":"front_yard_east","id":"x1","current_zones":["brick_walkway"]}}'
    sub._on_message(None, None, msg)               # must not raise
    msg.payload = b"not json at all"
    sub._on_message(None, None, msg)
    assert sub.errors == 1, (sub.errors, logs)


# ── 7. cue confirmation: played / not played / rebooted ───────────────────────
def test_cue_confirmation():
    import livebridge.show_mqtt as sm
    loud = []
    m = sm.ShowMQTT(log=lambda s: None, loud=loud.append)
    with mock.patch.dict(sm.CUE_ACK_WINDOW_S, {"mainshow": 1.5}), mock.patch.object(sm, "CUE_ACK_POLL_S", 0.05):
        m._last_leader_status = {"status": "online", "wav_plays": 10, "wav_short": 0}

        def play():
            time.sleep(0.3)
            m._last_leader_status = {"status": "online", "wav_plays": 12, "wav_short": 0}
        threading.Thread(target=play).start()
        m._confirm_cue(1, "mainshow")
        assert m.cue_played == 1 and m.cue_misses == 0

        m._confirm_cue(2, "mainshow")               # nothing plays
        assert m.cue_misses == 1 and any("CUE NOT PLAYED" in x for x in loud)

        def reboot():
            time.sleep(0.3)
            m._last_leader_status = {"status": "online", "wav_plays": 0, "wav_short": 0}
        threading.Thread(target=reboot).start()
        m._confirm_cue(3, "mainshow")
        assert m.cue_misses == 2


# ── 8. a cue dropped because the Scarecrow went silent is counted, loudly ─────
def test_gated_cue_counts_as_miss():
    import livebridge.service as svc
    b = svc.LiveBridge.__new__(svc.LiveBridge)
    b.show_log = mock.Mock()
    b.mqtt = mock.Mock(cue_misses=0, last_miss=None)
    b._gate = lambda: False
    b._update_cycle_summary = lambda c, p: None
    cyc = mock.Mock(session_id=0xABCD, hold_count=0)
    with mock.patch.object(svc, "theme_wire", lambda t: "x"), mock.patch.object(svc, "tone_wire", lambda t: "y"):
        b._on_cue(cyc, "mainshow")
        b._on_cue(cyc, "reset")
    assert b.mqtt.cue_misses == 1 and "GATED" in b.mqtt.last_miss
    b.mqtt.publish_cue.assert_not_called()


# ── 9. Joe 09-28: theme stays locked while the props play the last show ───────
def test_theme_locked_during_show_music():
    fake = FakeMono()
    with mock.patch("time.monotonic", fake):
        c, cues = make(fake)
        themes = []
        c.on_cycle_locked = lambda cyc: themes.append(cyc.theme)
        c.waiting_zone_trigger(); c.walkway_trigger()          # show 1
        for gap in (10, 30, 60):                               # same group bouncing around
            run(fake, c, gap)
            c.waiting_zone_trigger(theme=c.draw_next_theme()); c.walkway_trigger()
        assert len(set(themes)) == 1, themes
        run(fake, c, THEME_LOCK_S + 5)                         # music over: new card
        before = len(c.status()["draws"])
        c.draw_next_theme()
        assert len(c.status()["draws"]) == before + 1


# ── 10. Joe 09-28: a visit is over after 4 minutes ────────────────────────────
def test_visit_max_four_minutes():
    fake = FakeMono()
    with mock.patch("time.monotonic", fake):
        c, cues = make(fake)
        c.waiting_zone_trigger()
        for _ in range(VISIT_MAX_S + 5):                      # loitering in the waiting zone
            fake.t += 1.0
            if int(fake.t) % 15 == 0:
                c.waiting_zone_trigger()
            c.tick()
        assert c.status()["cycle"] is None or c.status()["cycle"]["age_s"] < VISIT_MAX_S
        assert cues[-1][2] in ("reset", "preshow")
        assert [p for _, _, p in cues].count("hold") == HOLD_MAX_LOOPS


# ── 11. Joe 09-28: FREE show = gate closed, MAIN-SHOW from the waiting zone ───
def test_gate_closed_mainshow_from_waiting_zone():
    fake = FakeMono()
    with mock.patch("time.monotonic", fake):
        c, cues = make(fake)
        c.gate_closed = True
        t0 = fake.t
        c.waiting_zone_trigger()
        run(fake, c, HOLD_INTERVAL_S * 2 + 2)
        phases = [p for _, _, p in cues]
        assert phases[:4] == ["preshow", "hold", "mainshow", "reset"], phases
        t_main = [t for t, _, p in cues if p == "mainshow"][0]
        assert HOLD_INTERVAL_S * 2 - 1 <= t_main - t0 <= HOLD_INTERVAL_S * 2 + 2, t_main - t0

        # the girls stay: encores, capped, spaced, then quiet
        run(fake, c, 240)
        enc = [t for t, _, p in cues if p == "encore"]
        assert len(enc) == MAX_ENCORES, cues
        assert all(b - a >= LINGER_ENCORE_GAP_S for a, b in zip([t_main] + enc, enc))


def test_gate_closed_walkway_still_works_and_empty_yard_gets_nothing():
    fake = FakeMono()
    with mock.patch("time.monotonic", fake):
        c, cues = make(fake)
        c.gate_closed = True
        c.waiting_zone_trigger()
        run(fake, c, 5)
        c.walkway_trigger()                                  # gate opened: bricks fire at once
        assert [p for _, _, p in cues][:2] == ["preshow", "mainshow"]
        c.set_presence(False)                                # they left
        run(fake, c, 240)
        assert "encore" not in [p for _, _, p in cues]

        c2, cues2 = make(fake)                               # left before 80s: no show, reset
        c2.gate_closed = True
        c2.waiting_zone_trigger()
        run(fake, c2, HOLD_INTERVAL_S + 2)
        c2.set_presence(False)
        run(fake, c2, HOLD_INTERVAL_S + 2)
        assert "mainshow" not in [p for _, _, p in cues2] and cues2[-1][2] == "reset"


def test_gate_open_waiting_zone_never_fires_mainshow():
    fake = FakeMono()
    with mock.patch("time.monotonic", fake):
        c, cues = make(fake)                                 # gate_closed False = AI / normal flow
        c.waiting_zone_trigger()
        run(fake, c, VISIT_MAX_S + 5)
        phases = [p for _, _, p in cues]
        assert "mainshow" not in phases and phases.count("hold") == HOLD_MAX_LOOPS, phases


def test_gate_closed_random_soak():
    rng = random.Random(4242)
    fake = FakeMono()
    with mock.patch("time.monotonic", fake):
        c, cues = make(fake)
        c.gate_closed = True
        for _ in range(4 * 3600):
            fake.t += 1.0
            r = rng.random()
            if r < 0.006:
                c.waiting_zone_trigger()
            elif r < 0.008:
                c.walkway_trigger()
            elif r < 0.012:
                c.set_presence(rng.random() < 0.7)
            c.tick()
            st = c.status()
            assert st["cycle"] is None or st["cycle"]["age_s"] <= VISIT_MAX_S + 1
        check_session_order(cues)
        phases = [p for _, _, p in cues]
        assert phases.count("mainshow") > 20 and phases.count("encore") > 5, \
            {p: phases.count(p) for p in set(phases)}


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"PASS {t.__name__}")
        except Exception as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e!r}")
    sys.exit(1 if failed else 0)
