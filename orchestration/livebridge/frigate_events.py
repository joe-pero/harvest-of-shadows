"""
frigate_events.py — the real `frigate/events` subscriber for the live bridge.

Modeled directly on the already-proven zone-diffing pattern in
`detect-server-build/frigate-audit/zone_timing_logger.py` (same broker, same
camera/zone set, bench-validated against this exact box already). Filters to
`camera in (front_yard_east, front_yard_west)` — both cameras are in the
trigger path as of 2026-09-21 (front_yard_west was briefly out per the
2026-09-08 spec; re-added with its own zone_one/zone_two, additive alongside
east's zones — see docs/show-trigger-geometry.md).

Zone -> ShowController call:
  sidewalk_zone_1 / sidewalk_zone_2 (east)   -> ctrl.prime_trigger()
  zone_two (west)                            -> ctrl.prime_trigger()
  waiting_zone (east)                        -> ctrl.waiting_zone_trigger(...)
  zone_one (west)                            -> ctrl.waiting_zone_trigger(...)
  brick_walkway (east only)                  -> ctrl.walkway_trigger()

Rate-limiting note (no AI call wired here yet — see middle_audio.py): a
waiting_zone "entered" event only triggers the (session_id, MIDDLE-hint)
callback when no cycle is currently active (`ctrl.status()["cycle"] is
None`). A re-enter on an already-active cycle just re-confirms presence
(matches the controller's own "never re-run pre-show" rule) — so repeated
Frigate zone-edge flicker for one visit costs zero extra AI calls by
construction, without needing a time-windowed dedup.

Presence for the HOLD loop (docs/show-trigger-geometry.md §2c step 1 — "is a
person still in waiting_zone / a sidewalk zone, read from the latest Frigate
state") is tracked live here via `current_presence()`, called from
service.py's tick loop right before `ctrl.tick()`.
"""
from __future__ import annotations

import json
import random
import sys
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Callable

import paho.mqtt.client as mqtt

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from mqtt_credentials import (  # noqa: E402
    FRIGATE_HTTP_URL,
    FRIGATE_MQTT_BROKER,
    MQTT_PASSWORD,
    MQTT_PORT,
    MQTT_USERNAME,
)
from showsim.constants import PRIME_ZONES, SHOW_CAMERAS, WAITING_ZONES, WALKWAY_ZONE  # noqa: E402
from showsim.controller import ShowController  # noqa: E402

FRIGATE_EVENTS_TOPIC = "frigate/events"
_PRESENCE_ZONES = PRIME_ZONES | WAITING_ZONES


def fetch_snapshot(event_id: str, timeout: float = 5.0) -> bytes | None:
    """GET the Frigate event snapshot. None on any failure — callers must
    treat a missing snapshot as a normal degraded case, not an error."""
    url = f"{FRIGATE_HTTP_URL}/api/events/{event_id}/snapshot.jpg"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read()
    except Exception:
        return None


class FrigateSubscriber:
    def __init__(
        self,
        ctrl: ShowController,
        waiting_zone_hint_fn: Callable[[str, object, int], object] | None,
        log: Callable[[str], None] = print,
        lock: threading.Lock | None = None,
        armed_fn: Callable[[], bool] | None = None,
        on_zone: Callable[[str, str, str], None] | None = None,
    ) -> None:
        """`waiting_zone_hint_fn(event_id, theme, session_id) -> ai_tone_hint
        | None` is called ONLY when no cycle is active yet, on a background
        worker thread (never the MQTT network thread — a slow AI/XTTS
        call must never stall our own keepalive, the exact bug found in
        Scarecrow's own firmware). `theme` and `session_id` are already
        committed (real deck card drawn, real wire id minted) by the time
        this is called — generate_middle()'s prompt needs the theme to build
        a theme-flavored line, and session_id lets the hint fn write the
        MIDDLE WAV straight to its final `middle/{session_id:08x}_sc.wav`
        path (no temp-file handoff needed) before returning. Both get passed
        again into the later waiting_zone_trigger(theme=...) call so nothing
        is drawn/minted twice. None (the default hint_fn) means "no AI wired
        up" — every new cycle falls back to FALLBACK_TONE, no MIDDLE line.
        Swapped for the real OpenAI+XTTS pipeline once reviewed (middle_audio.py).

        `lock` guards every ShowController call — shared with service.py's
        tick loop so the background AI thread and the periodic tick() never
        touch the controller at the same time. `armed_fn()` — the bridge's
        show switch (2026-09-28: show_mqtt.ShowMQTT.show_active = AI switch OR
        FREE show switch) — gates every dispatch: off means Frigate activity is
        still logged but never reaches the controller (no state advance, no
        publishes, no AI calls). Paid calls are gated separately by the AI
        switch in middle_audio.
        """
        self._ctrl = ctrl
        self._hint_fn = waiting_zone_hint_fn
        self._log = log
        self._lock = lock or threading.Lock()
        self._armed_fn = armed_fn or (lambda: True)
        self._on_zone = on_zone   # 2026-09-28: data for show improvement (every zone entry, show on or off)
        self._active: dict[tuple[str, str], set[str]] = {}   # (camera,obj_id) -> current zones
        self.errors = 0
        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="middle-ai")
        self._client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
        self._client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
        self._client.on_connect = self._on_connect
        self._client.on_message = self._on_message
        self._client.reconnect_delay_set(min_delay=1, max_delay=30)

    def start(self) -> None:
        self._client.connect(FRIGATE_MQTT_BROKER, MQTT_PORT, keepalive=30)
        self._client.loop_start()

    def stop(self) -> None:
        self._client.loop_stop()
        self._client.disconnect()
        self._executor.shutdown(wait=False)

    def current_presence(self) -> bool:
        """Union of every currently-tracked object's zones, either camera —
        True if anyone is in a PRE-SHOW zone or a PRIME zone."""
        for zones in self._active.values():
            if zones & _PRESENCE_ZONES:
                return True
        return False

    def _on_connect(self, client, userdata, flags, rc, properties=None) -> None:
        self._log(f"[frigate] connected rc={rc} — subscribing {FRIGATE_EVENTS_TOPIC}")
        client.subscribe(FRIGATE_EVENTS_TOPIC)

    def _on_message(self, client, userdata, msg) -> None:
        # 2026-09-28: paho re-raises callback exceptions in its network thread,
        # which kills it: every later Frigate trigger would be silently lost
        # while the service still looks "running". Never let one escape.
        try:
            self._handle(msg)
        except Exception as e:
            self.errors += 1
            self._log(f"!!! [frigate] handler error #{self.errors} (event dropped, subscriber alive): {e!r}")

    def _handle(self, msg) -> None:
        try:
            d = json.loads(msg.payload)
        except Exception as e:
            self._log(f"[frigate] parse error: {e}")
            return

        after = d.get("after") or {}
        camera = after.get("camera")
        if camera not in SHOW_CAMERAS:
            return

        ev_type = d.get("type")
        obj_id = after.get("id")
        event_id = after.get("id")   # Frigate's tracked-object id == the event id for snapshot lookup
        if obj_id is None:
            return
        key = (camera, obj_id)

        new_zones = set(after.get("current_zones") or [])
        if ev_type == "end":
            new_zones = set()
        old_zones = self._active.get(key, set())
        entered = new_zones - old_zones

        if ev_type == "end":
            self._active.pop(key, None)
        else:
            self._active[key] = new_zones

        for zone in sorted(entered):
            if self._on_zone is not None:
                self._on_zone(camera, zone, obj_id)
            self._dispatch(zone, event_id)

    def _dispatch(self, zone: str, event_id: str | None) -> None:
        if not self._armed_fn():
            self._log(f"[frigate] SHOW OFF (AI + FREE switches off) — {zone} entered, not forwarded to the controller")
            return

        if zone in PRIME_ZONES:
            self._log(f"[frigate] PRIME  {zone}")
            with self._lock:
                self._ctrl.prime_trigger()
            return

        if zone in WAITING_ZONES:
            with self._lock:
                active = self._ctrl.status()["cycle"] is not None
            if active or not self._hint_fn or not event_id:
                self._log(f"[frigate] {zone} re-enter (active={active}) — re-confirm only")
                with self._lock:
                    self._ctrl.waiting_zone_trigger()
                return
            self._log(f"[frigate] {zone} NEW — event={event_id} — handing off to AI worker thread")
            self._executor.submit(self._run_hint_and_trigger, event_id)
            return

        if zone == WALKWAY_ZONE:
            self._log(f"[frigate] MAIN-SHOW {zone}")
            with self._lock:
                self._ctrl.walkway_trigger()

    def _run_hint_and_trigger(self, event_id: str) -> None:
        """Runs off the MQTT network thread — a slow AI/XTTS call here
        must never delay our own keepalive or block a PRIME/walkway event
        from a different visitor elsewhere in the yard."""
        with self._lock:
            theme = self._ctrl.draw_next_theme()
        session_id = random.getrandbits(32)

        t0 = time.monotonic()
        try:
            hint = self._hint_fn(event_id, theme, session_id)
        except Exception as e:
            self._log(f"[frigate] MIDDLE hint fn crashed: {e!r} — falling back to default tone")
            hint = None
        self._log(f"[frigate] MIDDLE hint fn took {time.monotonic() - t0:.1f}s -> hint={hint}")

        with self._lock:
            if not self._armed_fn():
                self._log("[frigate] show switched off while the AI call was in flight — dropping this trigger "
                          f"(deck card {theme.name} was drawn and stays spent)")
                return
            if self._ctrl.status()["cycle"] is not None:
                self._log("[frigate] another cycle started while the AI call was in flight — re-confirm only "
                          f"(deck card {theme.name} was drawn and stays spent)")
                self._ctrl.waiting_zone_trigger()
                return
            self._ctrl.waiting_zone_trigger(ai_tone_hint=hint, session_id=session_id, theme=theme)
