"""
show_mqtt.py — the real harvest/show/* MQTT transport for the live bridge.

Wire contract, ground truth = props/scarecrow-narrator-a1s/src/mqtt_client.cpp
(locked 2026-09-11, see docs/_history/HANDOFF-2026-09-11-mqtt-cue-bridge.md):

  harvest/show/params   retained  {"session_id","theme","tone","cycle"}
  harvest/show/cue      NOT retained  {"session_id","phase"}
  harvest/show/leader/status  retained, published by Scarecrow (we subscribe)
    {"status","show_active","cue_ok","cue_fail","burt_acks"}

Hard rules from the contract (do not violate — the firmware silently no-ops
otherwise, per its own s_haveParams guard):
  - params MUST be published (retained) before the first cue of a new cycle.
  - Do NOT republish params on a HOLD retrigger.
  - Clear retained params (empty payload) at "reset".
  - theme/tone strings are exact: "corn"|"rotld"|"pumpkins",
    "friendly"|"scary"|"intense" — any typo silently defaults to friendly.

Reliability (docs/a1s-board-and-audio-architecture.md §3.4 H4 + this
session's own design, per Joe's "I want this to just work every time"):
  - If no leader/status for LEADER_SILENCE_S, stop driving cues (still log
    Frigate activity) and log the outage loudly; resume automatically.
  - After a mainshow cue, wait briefly for cue_ok to advance (or show_active
    to flip); if it doesn't, log loudly (visible incident, not silent).
    Deliberately NOT auto-retried: a cue publish isn't idempotent on the
    firmware side (a duplicate mainshow would re-fire the whole OUTRO/prop
    sequence), so a blind retry risks a worse failure than a missed ack —
    the miss might just be a delayed leader/status heartbeat, not a dropped
    cue. Surface it loudly instead and let a human judge.
"""
from __future__ import annotations

import json
import sys
import threading
import time
from pathlib import Path
from typing import Callable

import paho.mqtt.client as mqtt

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from mqtt_credentials import MQTT_BROKER, MQTT_PASSWORD, MQTT_PORT, MQTT_USERNAME  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from showsim.constants import ShowMode, ShowTheme  # noqa: E402
from showsim.controller import Cycle  # noqa: E402

PARAMS_TOPIC = "harvest/show/params"
CUE_TOPIC = "harvest/show/cue"
LEADER_STATUS_TOPIC = "harvest/show/leader/status"

# Bridge-side master switch — Joe working in the yard, physically triggering
# zones, needs a way to pause everything (cues AND AI calls) without killing
# the process. Defaults to DISARMED (safe) until explicitly armed:
#   mosquitto_pub -h 192.168.1.x -u halloween -P <password> \
#     -t harvest/bridge/mode -r -m armed
#
# 2026-09-18: this topic is retained, which used to mean "a restart remembers
# the last mode" (deliberate, at the time). Now that a real per-call cost is
# on the line (OpenAI), that became a real risk -- the bridge got restarted
# ~6 times in one test session and silently came back ARMED every time via
# the stale retained value, with nobody deciding that in the moment. Every
# process start now force-publishes DISARMED (retained) on connect, before
# subscribing -- overwrites whatever was retained, so a restart is always a
# deliberate, fresh re-arm, never an inherited one. Manually arming during a
# session still works exactly as before; only a restart resets it.
BRIDGE_MODE_TOPIC = "harvest/bridge/mode"
# 2026-09-24: live AI gate/spend snapshot for the garage dashboard's Narration
# tile (retained, JSON, refreshed every AI_STATUS_EVERY_S by service.py).
AI_STATUS_TOPIC = "harvest/bridge/ai_status"
# 2026-09-28: second switch, the FREE (prerecorded) show. Joe: two versions of
# the show, two dashboard buttons. harvest/bridge/mode (above) is now the AI
# switch only (paid OpenAI calls); this one lets Frigate triggers play the
# prerecorded show (SD INTRO/OUTRO, bank MIDDLE, Burt) with no paid call.
# Frigate triggers reach the props when EITHER switch is on (the AI show needs
# the show running too). Retained "on"/"off". Unlike the AI switch it is NOT
# forced off on connect: it costs nothing, and a bridge restart mid-evening
# (e.g. the end of a render queue) shouldn't silently kill the show.
SHOW_TOPIC = "harvest/bridge/show"

LEADER_SILENCE_S = 90          # docs/a1s-board-and-audio-architecture.md §3.4 H4
CUE_ACK_POLL_S = 0.5
# 2026-09-28 hardening: a cue counts as delivered when the Scarecrow's own
# wav_plays counter goes up (it publishes leader/status on every cue
# completion + every 30s). The old check waited 8s for cue_ok, but cue_ok
# counts Burt ESP-NOW handshakes, not cues, and a MAIN-SHOW banter runs well
# past 8s, so every MAIN-SHOW logged a false "possible miss" (and the wait
# blocked the controller lock). Windows = longest real cue + one heartbeat.
CUE_ACK_WINDOW_S = {"preshow": 90.0, "hold": 60.0, "mainshow": 150.0, "encore": 120.0}

_THEME_WIRE = {ShowTheme.CORN: "corn", ShowTheme.ROTLD: "rotld", ShowTheme.MUTANT_PUMPKINS: "pumpkins"}
_TONE_WIRE = {ShowMode.FRIENDLY: "friendly", ShowMode.SCARY: "scary", ShowMode.INTENSE: "intense"}


def theme_wire(theme: ShowTheme) -> str:
    return _THEME_WIRE[theme]


def tone_wire(tone: ShowMode) -> str:
    return _TONE_WIRE[tone if tone in _TONE_WIRE else ShowMode.SCARY]


class ShowMQTT:
    """Owns the broker connection: publishes harvest/show/{params,cue} for
    real, and watches harvest/show/leader/status for the reliability gate."""

    def __init__(
        self,
        log: Callable[[str], None] = print,
        loud: Callable[[str], None] | None = None,
        show_log=None,
    ) -> None:
        self._log = log
        self._loud = loud or (lambda m: log(f"!!! {m}"))
        self._show_log = show_log
        self._client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
        self._client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
        self._client.on_connect = self._on_connect
        self._client.on_message = self._on_message
        self._client.reconnect_delay_set(min_delay=1, max_delay=30)

        self._connected = False
        self._last_leader_status: dict | None = None
        self._last_leader_seen: float | None = None
        self._cycles_with_params: set[int] = set()   # session_ids we've latched params for
        self._armed = False   # safe default — must be explicitly armed
        self._show_on = False  # FREE show switch; set from the retained value on subscribe
        self.cue_sent = 0       # audio cues published (preshow/hold/mainshow/encore)
        self.cue_played = 0     # ...confirmed by a wav_plays increase
        self.cue_misses = 0     # ...with NO new playback inside the window (loud)
        self.last_miss: str | None = None

    # ── lifecycle ────────────────────────────────────────────────────────
    def connect(self) -> None:
        self._client.connect(MQTT_BROKER, MQTT_PORT, keepalive=30)
        self._client.loop_start()

    def stop(self) -> None:
        self._client.loop_stop()
        self._client.disconnect()

    def _on_connect(self, client, userdata, flags, rc, properties=None) -> None:
        self._connected = rc == 0
        self._log(f"[mqtt] connected rc={rc} — forcing DISARMED (clears any stale "
                  f"retained mode from a previous run), then subscribing "
                  f"{LEADER_STATUS_TOPIC}, {BRIDGE_MODE_TOPIC}")
        self._armed = False
        client.publish(BRIDGE_MODE_TOPIC, "disarmed", qos=1, retain=True)
        client.subscribe(LEADER_STATUS_TOPIC)
        client.subscribe(BRIDGE_MODE_TOPIC)
        client.subscribe(SHOW_TOPIC)

    def _on_message(self, client, userdata, msg) -> None:
        if msg.topic == BRIDGE_MODE_TOPIC:
            mode = (msg.payload or b"").decode(errors="replace").strip().lower()
            was = self._armed
            self._armed = mode == "armed"
            if self._armed != was:
                self._log(f"[mqtt] bridge mode -> {'ARMED' if self._armed else 'DISARMED'} (payload={mode!r})")
            return
        if msg.topic == SHOW_TOPIC:
            val = (msg.payload or b"").decode(errors="replace").strip().lower()
            was = self._show_on
            self._show_on = val == "on"
            if self._show_on != was:
                self._log(f"[mqtt] FREE show -> {'ON' if self._show_on else 'OFF'} (payload={val!r})")
            return
        if msg.topic != LEADER_STATUS_TOPIC:
            return
        try:
            st = json.loads(msg.payload) if msg.payload else None
        except Exception as e:
            self._log(f"[mqtt] leader/status parse error: {e}")
            return
        self._last_leader_status = st
        # 2026-09-25: the broker's LWT {"status":"offline"} (and the retained
        # copy replayed on subscribe) is NOT a sign of life -- it used to reset
        # the silence timer, so a dead Scarecrow looked alive for 90 more s.
        if isinstance(st, dict) and st.get("status") == "offline":
            if self._last_leader_seen is not None:
                self._loud("Scarecrow reported OFFLINE (MQTT LWT) -- cues will pause")
            self._last_leader_seen = None
            return
        self._last_leader_seen = time.time()

    def armed(self) -> bool:
        """AI switch (harvest/bridge/mode): gates paid OpenAI calls only."""
        return self._armed

    def show_on(self) -> bool:
        """FREE show switch (harvest/bridge/show)."""
        return self._show_on

    def show_active(self) -> bool:
        """Frigate triggers reach the props: either switch on."""
        return self._armed or self._show_on

    def force_disarm(self, reason: str) -> None:
        """Hard safety trip -- e.g. the OpenAI lifetime spend cap. Publishes
        DISARMED (retained) so the detect-server dashboard reflects it too,
        not just this process's in-memory state. Requires a real, deliberate
        re-arm afterward; nothing auto-clears this."""
        self._armed = False
        self._loud(f"FORCE DISARM: {reason}")
        self._client.publish(BRIDGE_MODE_TOPIC, "disarmed", qos=1, retain=True)

    # ── reliability ──────────────────────────────────────────────────────
    def leader_silent(self) -> bool:
        """True if we've never heard leader/status, or it's gone stale."""
        if self._last_leader_seen is None:
            return True
        return (time.time() - self._last_leader_seen) >= LEADER_SILENCE_S

    def leader_status(self) -> dict | None:
        return self._last_leader_status

    def _plays(self) -> tuple[int, int] | None:
        """(wav_plays, wav_short) from the last live leader/status, or None."""
        st = self._last_leader_status
        if not isinstance(st, dict) or st.get("status") != "online" or "wav_plays" not in st:
            return None
        return (int(st.get("wav_plays", 0)), int(st.get("wav_short", 0)))

    # ── publish ──────────────────────────────────────────────────────────
    def publish_params(self, cycle: Cycle) -> None:
        payload = {
            "session_id": cycle.session_id,
            "theme": theme_wire(cycle.theme),
            "tone": tone_wire(cycle.tone),
            "cycle": cycle.number,
        }
        # 2026-09-24 banter playback (docs/DESIGN-outro-banter-playback.md):
        # which exchange the boards play at MAIN-SHOW / encore. Absent = the
        # firmware's own OUTRO pool (fallback). Old firmware ignores both keys.
        if getattr(cycle, "banter", None):
            payload["banter"] = cycle.banter
        if getattr(cycle, "encore", False):
            payload["encore"] = True
        # 2026-09-25: tell the Scarecrow whether a MIDDLE is coming. hint_for()
        # returns None on every path that produces no MIDDLE (AI off / out of
        # hours / cap / no snapshot / failed read) and a clip always lands
        # otherwise, so the locked AI hint IS that answer. false = the firmware
        # skips its 30s post-INTRO wait (was 30s of dead air every gated visit).
        # Old firmware ignores the key.
        payload["middle"] = (not getattr(cycle, "encore", False)
                             and getattr(cycle, "ai_hint_at_lock", None) is not None)
        self._client.publish(PARAMS_TOPIC, json.dumps(payload), qos=0, retain=True)
        self._cycles_with_params.add(cycle.session_id)
        self._log(f"[mqtt] -> {PARAMS_TOPIC} (retained) {payload}")

    def publish_nomiddle(self, session_id: int) -> None:
        """2026-09-25: MIDDLE was promised (params middle=true) but assembly
        failed -- ends the Scarecrow's post-INTRO wait now instead of at 30s.
        Not a show phase; firmware never queues it."""
        payload = {"session_id": session_id, "phase": "nomiddle"}
        self._client.publish(CUE_TOPIC, json.dumps(payload), qos=0, retain=False)
        self._log(f"[mqtt] -> {CUE_TOPIC} {payload}")

    def publish_ai_status(self, status: dict) -> None:
        self._client.publish(AI_STATUS_TOPIC, json.dumps(status), qos=0, retain=True)

    def clear_params(self) -> None:
        self._client.publish(PARAMS_TOPIC, payload=None, qos=0, retain=True)
        self._log(f"[mqtt] -> {PARAMS_TOPIC} (retained CLEAR)")

    def publish_cue(self, cycle: Cycle, phase: str, *, confirm: bool = False) -> None:
        if cycle.session_id not in self._cycles_with_params and phase != "reset":
            self._loud(
                f"cue phase={phase} for session {cycle.session_id:08x} has no params "
                f"latched yet — firmware will silently drop this (s_haveParams guard). "
                f"Publishing anyway; this is a bridge bug if it happens."
            )
        payload = {"session_id": cycle.session_id, "phase": phase}
        self._client.publish(CUE_TOPIC, json.dumps(payload), qos=0, retain=False)
        self._log(f"[mqtt] -> {CUE_TOPIC} {payload}")

        if phase == "reset":
            self._cycles_with_params.discard(cycle.session_id)
            self.clear_params()

        if confirm and phase in CUE_ACK_WINDOW_S:
            self.cue_sent += 1
            threading.Thread(target=self._confirm_cue, args=(cycle.session_id, phase),
                             name=f"cue-ack-{phase}", daemon=True).start()

    def _confirm_cue(self, session_id: int, phase: str) -> None:
        """Background thread (never holds the controller lock): did the
        Scarecrow actually play anything for this cue? Success = wav_plays
        went up. A wav_short increase (clip cut off) is loud too. Rebooted
        board (counter went DOWN) = loud miss. Best-effort: no per-cue ack
        exists on the wire yet (firmware follow-up in CURRENT-BUILD)."""
        window = CUE_ACK_WINDOW_S[phase]
        before = self._plays()
        deadline = time.time() + window
        after = before
        while time.time() < deadline:
            time.sleep(CUE_ACK_POLL_S)
            after = self._plays()
            if before is None:
                before = after          # no live status at publish time: baseline on the first one
                continue
            if after is None:
                continue
            if after[0] < before[0]:
                break                   # counter reset = Scarecrow rebooted mid-cue
            if after[0] > before[0]:
                self.cue_played += 1
                if after[1] > before[1]:
                    self._loud(f"cue phase={phase} session {session_id:08x}: a clip was CUT SHORT "
                               f"(wav_short {before[1]} -> {after[1]})")
                return
        self.cue_misses += 1
        self.last_miss = f"{time.strftime('%H:%M:%S')} {phase}"
        self._loud(
            f"CUE NOT PLAYED: phase={phase} session {session_id:08x} -- no new Scarecrow playback "
            f"within {window:.0f}s (wav_plays before={before}, after={after}). Check the prop."
        )
        if self._show_log is not None:
            self._show_log.log("cue_not_played", session_id=session_id, phase=phase,
                               plays_before=before, plays_after=after, window_s=window)
