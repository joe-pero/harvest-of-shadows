# touchpad-ota-controller

The show's HUD + manual override panel for the Zombie Container.
`NODE_OTA_CONTROLLER` (0x0B). Hosyond 4.0″ ESP32-32E ST7796S
resistive-touch board, **MAC `AA:BB:CC:DD:EE:FF`**.

> **WORKING — 2026-09-21.** Touch UI redesigned to a single full-screen
> override button (no more split zones/bucket menu) — bench-verified: 88
> taps across the whole panel, zero dead zones. **This board is no longer
> the Zombie Container's real trigger** — that's now
> `props/zombie-relay-esp01s/` (a self-contained ESP-01S relay board), which
> listens directly for Scarecrow's ROTLD broadcast. This panel's fire still
> broadcasts `CMD_SEGMENT_START` as a secondary "backdoor" manual override,
> proven live end-to-end tonight. Full story: `docs/zombie-container.md` +
> `PROJECT_LOG.md` 2026-09-21 entries. HUD now has **7 states** (added
> DOUSE + INCINERATE full-screen takeover events). No media/WiZ player.

```
pio run -e touchpad_ota_controller -t upload
pio device monitor -e touchpad_ota_controller
```

## What it does

- WiFi ch11 + ESP-NOW (listen + originate)
- **Fires on `CMD_SET_THEME = ROTLD`** (theme-lock, armed by default) —
  pulses its own local relay AND broadcasts `CMD_SEGMENT_START` for the
  ESP-01S relay board + Eave. (The local relay wiring on this board was
  abandoned 2026-09-21 after hours of real hardware bugs — see the repo-root
  docs — but the firmware path is unchanged; the pulse just doesn't drive
  anything physical from this board anymore.)
- **7-state model** (`dmx_show_state` 0–6, `dmx_timeline.h`): CONTAINED /
  WARNING / GLASS BREAK / BROKEN / DOUSE / INCINERATE / (RESET, bench-only —
  the live show's tail now maps straight back to CONTAINED).
- **RGB LED** (GPIO 22/16/17) per state: green swamp water / red double-thump
  heartbeat / solid strobe / broken-electrical fire+red / amber "dousing"
  pulse / continuous red-orange fire flicker.
- **HUD** (`hud.cpp`, LovyanGFX) — "DARROW CHEMICAL CO. // 2-4-5 TRIOXIN"
  containment monitor, landscape, phase-reactive. DOUSE and INCINERATE are
  full-screen takeover events (particle effects + big text), not the normal
  PPM/EKG/integrity panel layout — see `docs/zombie-container.md`.
- **HUD burn-in protection** — pixel orbit (±6 px / 90 s), dim CONTAINED,
  and a dim wandering STANDBY screensaver preview (`idle` cmd, bench-only).
- **Windowed OTA** (`ota_service.*`) — associates to ch-11 WiFi + runs
  `ArduinoOTA` ("halloween-touchpad-ota") only while an OTA window is open
  (`CMD_OTA_MODE` inbound, or the `ota` cmd), self-closes.
- `CMD_OTA_MODE` broadcast · e-stop (serial-only).

## Touch UI

Redesigned 2026-09-21 — one full-screen button, no split zones anywhere:
CONTAINED monitor → tap anywhere → full-screen "TAP TO FIRE" override →
tap anywhere → fires (relay pulse + DMX segment + `CMD_SEGMENT_START`
broadcast), jumps back to the monitor to watch it run. 30 s inactivity with
no tap returns to the monitor without firing. **Fully locked out while a
show is running** — the override screen can't be opened at all mid-show.
No ARM/DISARM toggle and no Singing Pumpkins jukebox on touch anymore
(dropped; arm/disarm are serial-only, same as `estop` always was).

## Serial

`arm` `disarm` · `fire` / `fire <sec>` (pre-advance the clock) · `estop` ·
`ota [min]` (broadcast + open own window) ·
`state contained|warning|glass|broken|douse|incinerate|reset` (freeze one) ·
`demo` (cycle all 7) · `idle` (preview the STANDBY screensaver, bench-only) ·
`theme <n>` (simulate a SET_THEME cue) · `status` · `trot <0-7>` (live touch
offset_rotation, bench-only) · `freeze` (bench-only: freezes the real
override screen so taps log without firing — see "Touch calibration" in
`docs/zombie-container.md`)

## Flagged open (see `docs/zombie-container.md`)

- ⚠ **"media/WiZ player"** — device unconfirmed. `media_player.*` is a no-op stub.
- ⚠ **The real trigger is now `props/zombie-relay-esp01s/`** — its primary
  Scarecrow-ROTLD-direct path is proven in code but not yet fired by a real
  Scarecrow cue; its relay isn't yet wired to the actual PicoDMX trigger.
- ⚠ **Eave** — sender-gate + 5-state rework DONE 2026-09-15, confirmed still
  correct 2026-09-21; does NOT yet know about the new DOUSE/INCINERATE
  sub-phases (still just shows BROKEN's look through that whole window) —
  open follow-up if Joe wants Eave to mirror them too.
- ⚠ `segment_protocol.h` is now a third independent local copy (touchpad,
  Eave, the new ESP-01S node) — promotion to a shared header still open.

## Files

`espnow_rx` · `relay_trigger` · `dmx_state` (+ `dmx_timeline.h`) · `hud` (+
`lgfx_conf.h`) · `ota_service` · `media_player` (stub) · `segment_protocol.h` ·
`pins.h` · `main.cpp`. Bench logs in `bench/`.
