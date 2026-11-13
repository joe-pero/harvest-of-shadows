#!/usr/bin/env python3
"""
service.py — the live bridge entry point. Wires:

  frigate_events.FrigateSubscriber  --triggers-->  showsim.controller.ShowController
                                                        |  on_cycle_locked / on_cue hooks
                                                        v
                                     show_mqtt.ShowMQTT --publishes--> harvest/show/{params,cue}
                                     show_mqtt.ShowMQTT <--subscribes-- harvest/show/leader/status

Also runs the static file server audio_bridge.py's --serve mode provides
(same directory, same port 8080) in-process, so this one service replaces
having to remember to separately start `audio_bridge.py --serve` too.

Run foreground for bench testing (see README.md for the mosquitto_pub/sub
verification recipe — no firmware reflash needed, matches the wire contract
already bench-verified on Scarecrow 2026-09-11). MUST run under
~/xtts-env/bin/python3 (has paho-mqtt + openai + TTS/torch all installed
together — the system python3 and orchestration/vision/.venv each only have
some of these):

    ~/xtts-env/bin/python3 orchestration/livebridge/service.py
    # or, if executable:
    orchestration/livebridge/service.py

STATUS 2026-09-18: the live MIDDLE/OUTRO pipeline (middle_audio.py) calls
OpenAI (vision/openai_middle.py; Gemini support removed this date — see
PROJECT_LOG.md "2026-09-18") then synthesizes the line live via XTTS-v2. No
OPENAI_API_KEY -> every cycle falls back to FALLBACK_TONE (Scary) with no
MIDDLE/OUTRO line, a fully supported path (firmware's own MIDDLE-fetch
timeout+pad handles a missing file cleanly, bench-verified). Rate limiting
(per-kind cooldown + daily cap + lifetime $ cap + schedule window + a real-429
backoff), the arm/disarm gate, and the structured audit log
(livebridge/logs/show_events.jsonl) are all real — see middle_audio.py's own
docstring and orchestration/livebridge/BUILD_LOG.md for the full design
history.
"""
from __future__ import annotations

import os
import signal
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ORCH = HERE.parent
sys.path.insert(0, str(ORCH))

ENV_FILE = HERE / ".env"


def _load_env_file() -> None:
    """OPENAI_API_KEY (etc.) from livebridge/.env, KEY=VALUE per line, never
    pasted into chat/logs — same pattern as frigate-audit's own
    _load_mqtt_password(). Does not override an already-exported env var."""
    if not ENV_FILE.exists():
        return
    for line in ENV_FILE.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        os.environ.setdefault(key.strip(), val.strip())


_load_env_file()

from showsim.controller import ShowController  # noqa: E402
from showsim.constants import MAX_CYCLE_AGE_S  # noqa: E402
from livebridge.frigate_events import FrigateSubscriber  # noqa: E402
from livebridge.show_mqtt import LEADER_SILENCE_S, ShowMQTT, theme_wire, tone_wire  # noqa: E402
from livebridge.middle_audio import MiddleAudioService  # noqa: E402
from livebridge.banter_select import BanterSelector  # noqa: E402
from livebridge.show_log import ShowLog  # noqa: E402


class LiveClock:
    """Real wall time for the live controller. 2026-09-28: the bridge used the
    controller's default SimClock, which only moves when narration "plays", so
    in the live show it froze after the first cycle: HOLD ticks and the idle
    reset never fired, and the 120s same-group / 50s encore windows never
    expired, so every later walkway trigger was dropped as a re-trigger.
    advance() is a no-op: the props speak the lines, the bridge must not
    sleep through them while holding the controller lock."""

    def __call__(self) -> float:
        return time.monotonic()

    def advance(self, seconds: float) -> None:
        pass


AUDIO_DIR = Path("/tmp/halloween_audio")
AUDIO_PORT = 8080

TICK_INTERVAL_S = 1.0
AI_STATUS_EVERY_S = 30   # dashboard Narration tile refresh (retained harvest/bridge/ai_status)
EVENTS_LOG = HERE / "logs" / "show_events.jsonl"


def _log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _loud(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] !!! {msg}", file=sys.stderr, flush=True)


class LiveBridge:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.show_log = ShowLog(EVENTS_LOG)
        self.mqtt = ShowMQTT(log=_log, loud=_loud, show_log=self.show_log)
        self.ctrl = ShowController(
            clock=LiveClock(),
            emit=_log,
            on_cycle_locked=self._on_cycle_locked,
            on_cue=self._on_cue,
        )
        self.banter = BanterSelector(log=_log, loud=_loud)
        self.middle = MiddleAudioService(log=_log, loud=_loud, show_log=self.show_log,
                                         disarm_fn=self.mqtt.force_disarm,
                                         armed_fn=self.mqtt.armed,
                                         no_middle_fn=self.mqtt.publish_nomiddle)
        self.subscriber = FrigateSubscriber(
            self.ctrl,
            waiting_zone_hint_fn=self.middle.hint_for,
            log=_log,
            lock=self.lock,
            armed_fn=self.mqtt.show_active,   # 2026-09-28: AI or FREE show switch
            on_zone=lambda cam, zone, obj: self.show_log.log("zone_enter", camera=cam, zone=zone, obj=obj),
        )
        self._was_silent = False
        self._last_presence: bool | None = None
        self._cycle_summaries: dict[int, dict] = {}   # session_id -> in-progress summary, see _on_cue

    # ── controller hooks -> real MQTT ───────────────────────────────────
    def _gate(self) -> bool:
        """True if we should actually publish. False = leader silent; log
        once per transition, keep the state machine running locally, don't
        spam a broker nobody's listening on."""
        silent = self.mqtt.leader_silent()
        if silent and not self._was_silent:
            _loud(
                f"no harvest/show/leader/status for >= {LEADER_SILENCE_S}s — pausing cue "
                f"publishing. Frigate triggers are still logged."
            )
            self.show_log.log("leader_silence", state="paused")
        elif not silent and self._was_silent:
            _log("leader/status resumed — cue publishing re-armed")
            self.show_log.log("leader_silence", state="resumed")
        self._was_silent = silent
        return not silent

    def _on_cycle_locked(self, cycle) -> None:
        # Pick the OUTRO banter exchange now: it rides in the retained params,
        # which must be latched on the board before any cue for this session.
        cycle.banter = self.banter.pick(tone_wire(cycle.tone), encore=cycle.encore)
        self.show_log.log("banter_pick", session_id=cycle.session_id,
                          encore=cycle.encore, banter=cycle.banter)
        if cycle.encore:
            # banter-only encore: no visit summary of its own (the visit's
            # summary was written at its MAIN-SHOW reset)
            if self._gate():
                self.mqtt.publish_params(cycle)
            else:
                _log(f"[gated] would publish encore params for session {cycle.session_id:08x}")
            return
        self.show_log.log(
            "cycle_locked",
            session_id=cycle.session_id,
            theme=theme_wire(cycle.theme),
            tone=tone_wire(cycle.tone),
            cycle_number=cycle.number,
            ai_tone_hint=cycle.ai_hint_at_lock.name if cycle.ai_hint_at_lock else None,
            manual_override=cycle.manual_at_lock.name if cycle.manual_at_lock else None,
        )
        self._cycle_summaries[cycle.session_id] = {
            "session_id": cycle.session_id,
            "theme": theme_wire(cycle.theme),
            "tone": tone_wire(cycle.tone),
            "gemini_ok": cycle.ai_hint_at_lock is not None,
            "manual_override": cycle.manual_at_lock is not None,
            "started_at": time.time(),
            "reached_mainshow": False,
        }
        if self._gate():
            self.mqtt.publish_params(cycle)
        else:
            _log(f"[gated] would publish params for session {cycle.session_id:08x}")

    def _on_cue(self, cycle, phase: str) -> None:
        self.show_log.log(
            "cue", session_id=cycle.session_id, phase=phase, hold_count=cycle.hold_count,
            theme=theme_wire(cycle.theme), tone=tone_wire(cycle.tone),
            trigger=getattr(cycle, "trigger", None), source=getattr(cycle, "mainshow_source", None),
        )
        self._update_cycle_summary(cycle, phase)
        if self._gate():
            self.mqtt.publish_cue(cycle, phase, confirm=(phase != "reset"))
        elif phase == "reset":
            _log(f"[gated] would publish cue phase={phase} session {cycle.session_id:08x}")
        else:
            # 2026-09-28: a visitor just lost this part of the show. Count it
            # as a miss so show_health / the dashboard see it, not a quiet log.
            self.mqtt.cue_misses += 1
            self.mqtt.last_miss = f"{time.strftime('%H:%M:%S')} {phase} (GATED: Scarecrow silent)"
            _loud(f"CUE DROPPED: phase={phase} session {cycle.session_id:08x} -- Scarecrow heartbeat "
                  f"silent >= {LEADER_SILENCE_S}s, nothing was sent. Check the prop.")

    def _update_cycle_summary(self, cycle, phase: str) -> None:
        """Rolls up the per-cue-event stream into one row per visit, logged
        at reset — total hold taunts, final outcome, whether the AI call
        succeeded, total duration. Purely an aggregation of data already in
        the cycle_locked/cue events above; correlate by session_id if you
        want the raw per-event detail behind any one summary row."""
        summary = self._cycle_summaries.get(cycle.session_id)
        if summary is None:
            return   # shouldn't happen (cycle_locked always precedes cue) — defensive only
        if phase == "mainshow":
            summary["reached_mainshow"] = True
            return
        if phase != "reset":
            return
        summary = self._cycle_summaries.pop(cycle.session_id)
        if summary["reached_mainshow"]:
            outcome = "completed"
        elif cycle.hold_count > 0:
            outcome = "left_during_hold"
        else:
            outcome = "abandoned"
        self.show_log.log(
            "cycle_summary",
            session_id=summary["session_id"],
            theme=summary["theme"],
            tone=summary["tone"],
            gemini_ok=summary["gemini_ok"],
            manual_override=summary["manual_override"],
            hold_count=cycle.hold_count,
            outcome=outcome,
            duration_s=round(time.time() - summary["started_at"], 1),
            trigger=getattr(cycle, "trigger", None),
            mainshow_source=getattr(cycle, "mainshow_source", None),
            gate_closed=self.ctrl.gate_closed,
        )

    # ── run loop ─────────────────────────────────────────────────────────
    def run(self) -> None:
        self.mqtt.connect()
        self.subscriber.start()
        self._start_audio_server()
        self.middle.ensure_loaded()   # just checks the key is present now — the
                                       # MIDDLE bank is pre-rendered, nothing to load

        _log(
            "live bridge running — Ctrl-C to stop. Starts DISARMED (safe default): "
            "arm it with `mosquitto_pub -h 192.168.1.x -u halloween -P <password> "
            "-t harvest/bridge/mode -r -m armed` (see livebridge/README.md)."
        )
        running = True

        def _stop(*_a):
            nonlocal running
            running = False

        signal.signal(signal.SIGINT, _stop)
        signal.signal(signal.SIGTERM, _stop)

        last_ai_status = 0.0
        try:
            while running:
                if time.time() - last_ai_status >= AI_STATUS_EVERY_S:
                    last_ai_status = time.time()
                    try:
                        st = self.middle.ai_status()
                        # 2026-09-25: "cues" = live leader-silence gate, so a
                        # paused bridge is visible without a trigger to log it.
                        with self.lock:
                            cs = self.ctrl.status()
                        st.update(armed=self.mqtt.armed(), show=self.mqtt.show_on(), ts=round(time.time()),
                                  bridge="running",
                                  cues="paused" if self.mqtt.leader_silent() else "open",
                                  # 2026-09-28: controller liveness for show_health.sh (a
                                  # frozen clock / stuck cycle is now visible, not silent)
                                  ctrl_state=cs["state"], ctrl_clock=cs["clock_s"],
                                  cycle_age_s=cs["cycle"]["age_s"] if cs["cycle"] else None,
                                  cue_sent=self.mqtt.cue_sent, cue_played=self.mqtt.cue_played,
                                  cue_misses=self.mqtt.cue_misses, last_miss=self.mqtt.last_miss)
                        self.mqtt.publish_ai_status(st)
                    except Exception as e:
                        _log(f"[ai_status] publish failed: {e!r}")
                with self.lock:
                    presence = self.subscriber.current_presence()
                    # 2026-09-28: compare against the CONTROLLER's value, not our
                    # last push: _reset_cycle() sets it back to True, which left
                    # it stale (an empty yard looked occupied to the encore rule).
                    if presence != self.ctrl.status()["person_present"]:
                        self.ctrl.set_presence(presence)
                    self._last_presence = presence
                    # FREE show with AI off = gate closed (Joe 09-28): MAIN-SHOW from
                    # the waiting zone + linger encores. Walkway works in every mode.
                    gate = self.mqtt.show_on() and not self.mqtt.armed()
                    if gate != self.ctrl.gate_closed:
                        self.ctrl.gate_closed = gate
                        _log(f"[mode] gate-closed mode {'ON (FREE show, AI off): MAIN-SHOW from the waiting zone' if gate else 'OFF: MAIN-SHOW on the brick walkway'}")
                        self.show_log.log("mode", gate_closed=gate)
                    self.ctrl.tick()
                    self._watchdog()
                time.sleep(TICK_INTERVAL_S)
        finally:
            try:   # tell the dashboard the bridge is going away (not a silent stale "running")
                self.mqtt.publish_ai_status({"bridge": "stopped", "armed": self.mqtt.armed(),
                                             "show": self.mqtt.show_on(),
                                             "ts": round(time.time())})
            except Exception:
                pass
            self.subscriber.stop()
            self.mqtt.stop()
            self.middle.close()

    def _watchdog(self) -> None:
        """Called under self.lock each tick. A cycle past MAX_CYCLE_AGE_S means
        the controller's own timeouts failed (the 09-28 frozen clock parked the
        yard on one visit all evening): force it closed, loudly."""
        cs = self.ctrl.status()
        if cs["cycle"] and cs["cycle"]["age_s"] > MAX_CYCLE_AGE_S:
            c = self.ctrl.force_reset(f"cycle age {cs['cycle']['age_s']:.0f}s > {MAX_CYCLE_AGE_S}s watchdog")
            if c is not None:
                _loud(f"WATCHDOG: cycle {c.number} stuck {cs['cycle']['age_s']:.0f}s -- forced reset")
                self.show_log.log("watchdog_reset", session_id=c.session_id, age_s=cs["cycle"]["age_s"])
                self._on_cue(c, "reset")

    def _start_audio_server(self) -> None:
        sys.path.insert(0, str(ORCH / "voice"))
        from audio_bridge import start_server  # noqa
        start_server(AUDIO_DIR, AUDIO_PORT)
        (AUDIO_DIR / "middle").mkdir(parents=True, exist_ok=True)


def main() -> int:
    LiveBridge().run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
