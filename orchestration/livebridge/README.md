# orchestration/livebridge — the real Frigate → MQTT show-cue bridge

Replaces `orchestration/showsim/`'s simulator for a live show. Full design:
`docs/_history/HANDOFF-2026-09-11-mqtt-cue-bridge.md`, `BUILD_LOG.md` (this dir).

## Run it

**As a systemd service (installed, for real/unattended use):**

```bash
cd ~/Documents/harvest-of-shadows/orchestration
bash systemd/install.sh       # one-time: installs + starts harvest-livebridge.service
```
Auto-restarts on failure. Status/logs:
```bash
systemctl --user status harvest-livebridge.service
journalctl --user -u harvest-livebridge.service -f
# or: tail -f livebridge/logs/service.log
```
Survives a locked screen / SSH session, but **stops on a full logout**
unless lingering is enabled once: `sudo loginctl enable-linger $USER`
(`install.sh` tells you if this is needed).

**Foreground (bench testing):** run under `~/xtts-env/bin/python3` — it has
`paho-mqtt` + `google-genai` installed alongside the (currently unused at
runtime — see below) TTS/torch stack.
```bash
~/xtts-env/bin/python3 livebridge/service.py
```

Either way it starts **DISARMED** (safe default) — Frigate activity is
logged but never reaches the controller (no cues published, no AI calls)
until armed.

## Arm / disarm

**2026-09-28: two switches.** `harvest/bridge/show` (`on`/`off`, dashboard **Free Show**) plays the
prerecorded show with no paid call; `harvest/bridge/mode` (`armed`/`disarmed`, dashboard **AI Show**,
formerly "Narration") now only gates the paid OpenAI calls. Frigate triggers reach the controller when
EITHER is on. Only `mode` is forced DISARMED at bridge start; `show` keeps its retained value (costs nothing).
Everything below about `mode` still holds for the AI switch. `tools/show_health.sh` reports both.
**2026-10-20:** `show` is also switched by `tools/free_show_schedule.py` (user timer `harvest-free-show.timer`):
ON at sunset, OFF 20:30 / 21:30 Fri+Sat, edge-triggered (a manual dashboard flip wins until the next edge), skips
HA `input_text.show_skip_dates`, never touches it from Halloween 03:00 (manual night). Linger is on, so the bridge and
that timer start at boot without a login.

**From the detect-server dashboard** (added 2026-09-12): a toggle tile at
<http://detect-server:8080/dashboard.html>, next to the thermostat control.
Shows live state (not a stale poll) and works through the exact same topic
below — see `detect-server-build/info-dashboard/serve.py` and that
project's own docs for its half of this.

**Or directly**, retained MQTT topic `harvest/bridge/mode`, payload `armed` or `disarmed`:

```bash
# arm (drive the real show)
mosquitto_pub -h 192.168.1.x -u halloween -P <password> \
  -t harvest/bridge/mode -r -m armed

# disarm (Joe working in the yard, or anytime you want a hard pause)
mosquitto_pub -h 192.168.1.x -u halloween -P <password> \
  -t harvest/bridge/mode -r -m disarmed
```

(`mode` is forced DISARMED on every bridge start, see above.) **Turn BOTH switches off before working in
the yard** — otherwise your own movement through the PRIME/waiting zones
burns real AI calls and can fire real cues at Scarecrow if it's connected.

## Live MIDDLE/OUTRO lines (OpenAI vision call + live XTTS synthesis)

> **New build pending (2026-09-24):** the show is moving to a pre-recorded
> template + headcount + short live insert MIDDLE and a banter OUTRO. See
> `docs/CURRENT-BUILD.md`. This section describes what the bridge runs **today**
> until that wiring lands.

**What runs today.** One OpenAI
call (`orchestration/vision/openai_middle.py`) reads the visitor's photo at
PRESHOW start and returns `tone` + a `villain_line`; that line is synthesized
live via XTTS-v2 (`live_tts.py`, warmed up once at bridge startup, not
per-visitor) into `middle/{session_id:08x}_sc.wav`. The SAME trigger also
fires two more OpenAI text-only calls reusing that observation: Burt's
"flavor" reaction (`{session}_bu.wav`) and the AI-outro payoff pair for both
characters (`{session}_sc_outro.wav` / `_bu_outro.wav`) — see
`docs/_history/TASK-a1s-middle-parts-wiring.md` for the full design and current
firmware-side status on each board.

Off by default. To turn it on:

```bash
cp livebridge/.env.template livebridge/.env
nano livebridge/.env        # paste your real key after OPENAI_API_KEY=
systemctl --user restart harvest-livebridge.service   # or just restart the foreground process
```
`.env` is gitignored and only ever read locally by `service.py` — the key
never needs to be typed anywhere else, including chat. Get a key at
<https://platform.openai.com/api-keys>.

With no key set, every cycle just falls back to `FALLBACK_TONE` (Scary) with
no MIDDLE/flavor/outro lines — a fully supported path, not an error. All
three calls are gated by the SAME governor in `middle_audio.py` (class still
named `GeminiGovernor`, a legacy name; it governs the OpenAI calls)
(cooldown per call-kind, daily cap, a real lifetime $ cap, and a schedule
window) — see that file's module docstring for the current numbers.

**Re-rendering the bank** (e.g. to add more lines, or after editing
`voice/lines/scarecrow/middle/<bucket>.txt`):
```bash
cd ~/Documents/harvest-of-shadows/orchestration/voice
bash scripts/render_pools.sh scarecrow middle   # resumable — only renders what's missing
```
`MiddleBank` (in `middle_audio.py`) rescans lazily by mtime, so new lines
landing while the bridge is already running get picked up with no restart.

**GPU offload was evaluated and deliberately not wired in** (2026-09-11):
detect-server's Quadro P1000 does real synthesis in ~20s (vs ~49-69s CPU)
via a working CUDA env at `~/xtts-gpu-env` on that box, but (a) 20s still
can't beat the 4s window and (b) VRAM peaks at ~2.3GB against only ~2.4GB
free there, alongside Frigate's own detection model — a standing risk to
Frigate for a feature that still mostly wouldn't land in time. Left the env
in place, unused, in case that calculus changes later (shorter prompting,
more VRAM headroom, etc.) — nothing in `livebridge/` currently depends on it.

**OUTRO banter + walkway encore** (2026-09-24, `docs/DESIGN-outro-banter-playback.md`):
at cycle lock the bridge picks the OUTRO exchange (`banter_select.py`: tone pool,
no repeats all night, persisted in `state/banter_recent.json`) and sends it in the
retained params (`"banter": {...}`). A group still on the walkway within 120s of its
MAIN-SHOW gets a banter-only **encore** (same theme/tone, no deck draw, no props,
`encore` cue, max 2, ≥50s apart), drawn from the "still here?" set.
Scarecrow defers the `reset` cue that follows `mainshow` until its 210s SHOW_ACTIVE
window ends, so the props' theme phase isn't cut.

**Dashboard AI readout** (2026-09-24): the bridge publishes a retained JSON
snapshot on `harvest/bridge/ai_status` every 30s (`gate` =
open/outside_hours/budget_cap/daily_cap/backoff, `calls_today`, `spend_usd`,
`cap_usd`, `window`, `armed`, `bridge` running/stopped, `ts`); the garage
dashboard's Narration tile shows it next to the ARMED/OFF switch.

**Rate limiting and spend** (`middle_audio.py`): 15s minimum between MIDDLE
calls, 90s between OUTRO calls, a daily call cap (persisted in
`state/gemini_usage.json`, a legacy filename, resets at local midnight), a
5-minute backoff on a 429 from the API, the schedule window, and an **$18
lifetime hard cap** (`LIFETIME_BUDGET_USD`, persisted in
`state/gemini_lifetime_spend.json`) that force-disarms the show when hit.

## Structured logging (`logs/show_events.jsonl`) — for manual audit, not automation

One JSONL file, every event the whole pipeline produces, correlated by
`session_id`. Not automated in any way — plain text, no rotation, meant for
you to read by hand (or with `jq`/pandas) whenever you want. Event types:

| `event` | Fired from | Fields |
|---|---|---|
| `cycle_locked` | a new visitor locks a cycle | `session_id`, `theme`, `tone`, `cycle_number`, `ai_tone_hint`, `manual_override` |
| `gemini_call` (legacy name) | every real OpenAI vision attempt (or skip) | `session_id`, `event_id`, `decision` (`called`/`skip`/`error_429`), `reason`, `gemini_ms`, `ok`, `tone`, `tone_reason`, `fell_back`, `fallback_reason` |
| `cue` | every phase publish (`preshow`/`hold`/`mainshow`/`encore`/`reset`) | `session_id`, `phase`, `hold_count`, `theme`, `tone` |
| `banter_pick` | a cycle/encore locks | `session_id`, `encore`, `banter` (`{"pool","idx","lines"}` or null = boards use their OUTRO pool) |
| `cue_ack_miss` | a mainshow cue's confirm-check saw no change | `session_id`, `phase`, `cue_ok_before`, `cue_ok_after` |
| `leader_silence` | the 90s-silence gate flips | `state` (`paused`/`resumed`) |
| `cycle_summary` | one visit ends (`reset`) | `session_id`, `theme`, `tone`, `gemini_ok` (legacy name: AI call succeeded), `manual_override`, `hold_count`, `outcome` (`completed`/`left_during_hold`/`abandoned`), `duration_s` |
| `log_queue_full` | the logger itself dropped events | `dropped` (count) |

**Zero-latency guarantee**: `ShowLog.log()` (in `show_log.py`) only ever
does an in-memory, bounded-queue `put_nowait()` — never touches disk itself,
never raises. A separate background thread does the actual (best-effort)
file write. A stalled disk, a full log file, or a bug in the write path can
never delay or crash a real cue — see `show_log.py`'s own docstring for the
full guarantee and the one accepted tradeoff (a hard kill can lose a few
buffered rows).

Useful one-liners:
```bash
# everything for one visit, in order
jq -r 'select(.session_id==1811379370)' logs/show_events.jsonl

# how visits ended, this week
jq -r 'select(.event=="cycle_summary") | .outcome' logs/show_events.jsonl | sort | uniq -c

# AI (OpenAI) calls today, by decision
jq -r 'select(.event=="gemini_call") | .decision' logs/show_events.jsonl | sort | uniq -c

# real AI call latency
jq -r 'select(.event=="gemini_call" and .decision=="called") | .gemini_ms' logs/show_events.jsonl
```

## Verify without touching firmware

```bash
mosquitto_sub -h 192.168.1.x -u halloween -P <password> -t 'harvest/show/#' -v
```

Then, with the bridge armed, hand-publish a synthetic Frigate event to walk
the state machine (matches real Frigate's JSON shape — `camera` must be
`front_yard_east`, `current_zones` must include the target zone):

```bash
mosquitto_pub -h 192.168.1.x -u halloween -P <password> -t frigate/events -m \
  '{"type":"new","after":{"id":"test1","camera":"front_yard_east","label":"person","current_zones":["sidewalk_zone_1"]}}'

mosquitto_pub -h 192.168.1.x -u halloween -P <password> -t frigate/events -m \
  '{"type":"update","after":{"id":"test1","camera":"front_yard_east","label":"person","current_zones":["waiting_zone"]}}'
```

**Do not publish a `brick_walkway` test event if Scarecrow might be
powered on and connected** — that fires a real `mainshow` cue (real OUTRO
audio, real props). Check `harvest/show/leader/status`'s `"status"` field
first, or just make sure the board is off/disconnected before testing that
path.

## What's still pending

- A live test against Scarecrow with a real `mainshow` trigger — left for
  Joe / a session working directly at the board, per the handoff's scope.
  Every publish from this bridge has only been checked byte-for-byte against
  the documented wire contract via `mosquitto_sub`, never against a
  listening board from THIS bridge specifically (earlier hand-typed
  `mosquitto_pub` tests were done from a different session).
- **Burt (skeleton)'s microSD card seating — still open, NOT resolved.** An
  earlier report the same day said Burt booted fine with the card
  reinserted, but later that same evening, back at the bench, the identical
  GPIO12 zero-serial-with-card-in fault reconfirmed (2026-09-09 symptom,
  unchanged). Most consistent read: a genuinely intermittent DIP-1/card-
  seating fault, not actually fixed — don't rely on the earlier "resolved"
  report. Unrelated to this bridge's own code. Full detail:
  `props/skeleton-narrator-a1s/BUILD_LOG.md` 2026-09-11.
- **Burt's HOLD-cue handling — fixed 2026-09-11, not yet live-verified.**
  Separately from the card issue: Burt's old dedupe logic would have
  silently dropped every HOLD retrigger after the first cue in a cycle (not
  just ignored the HOLD flag cosmetically). Fixed and flashed
  (`props/skeleton-narrator-a1s/BUILD_LOG.md` 2026-09-11), but needs a live
  2-board test with a real `hold`×2-3 sequence to confirm before trusting it
  on show night — Scarecrow was offline when the fix was flashed.
- The MIDDLE bank hasn't been through the same "broken wind" post-process
  pass (denoise + ambient bed) that INTRO/OUTRO/HOLD got — `MiddleBank`
  currently serves `_raw_fx.wav` (FX-treated, not the final `_show.wav`
  polish). Cosmetic, not blocking; closeable later the same way those pools
  were finished.

## Resolved this session (see BUILD_LOG.md for full detail)

- Real cue-timing bug (live synthesis blocking the cue — see above) fixed
  and verified.
- Deployed as `harvest-livebridge.service` (systemd `--user`, auto-restart,
  always starts DISARMED).
- GPU offload to detect-server built and measured for real, then
  deliberately not adopted — see above.
