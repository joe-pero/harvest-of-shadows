# Fleet communication tiers

**Status: DESIGN — 2026-09-06. Classifies every prop by how much delivery
assurance its cue path actually needs. Companion to
`docs/a1s-board-and-audio-architecture.md` §3 (the A1S fleet-leader design) and
its §3.4 H1b (the Scarecrow↔Burt soft handshake this table generalises).**

---

## The question, per prop

> Does this prop's effect depend on staying **synchronised with another physical
> unit**, or does it just react **independently** to the fleet-wide broadcast on
> its own?

Default answer is **independent** (Tier 1). A prop only moves to Tier 2 when the
inventory shows a real, specific, multi-unit coordination need — named partner,
named reason.

## What each tier means

| Tier | Cue path | When |
|---|---|---|
| **Tier 0** | Not on the ESP-NOW show mesh at all | Different transport (MQTT), or standalone / bench, or not an effect prop |
| **Tier 1** | **Broadcast only.** Hears the fleet-wide `SET_THEME`/`SET_MODE`/`SHOW_STATE`/`START_SCENE` burst (sent 3×, no ACK — a1s doc §3.4 H1) and runs its own logic. | **Default.** Self-contained single-board prop; no other unit depends on its exact timing. |
| **Tier 2** | Broadcast (unchanged) **plus** a targeted **unicast** to a specific partner MAC, using ESP-NOW's **native** send-callback delivery result (`ESP_NOW_SEND_SUCCESS`/`FAIL`) — ≤3 closely-spaced tries, then proceed regardless. Never blocks. | Two (or more) physical units whose actions are **logically gated on each other** — Unit B must not start / advance until it knows Unit A got the cue or reached a point. |

**Tier 2 is a delivery *check*, not a clock.** The native unicast result answers
"did that node receive it?" It does **not** provide sub-frame timing sync. Props
that must *animate* in lockstep (a chase across light fixtures, say) are handled
by **coordinator-side sequencing** (timed `CmdPacket`s from Eave / a lighting
coordinator), not by unicast confirmation — and that's still Tier 1 here.

**Cost is not the constraint.** One unicast peer per Tier-2 relationship, well
under the 20-peer cap (the whole fleet is still reached through the single
`AA:BB:CC:DD:EE:FF` broadcast peer). The gate on Tier 2 is *complexity earning
its keep*, not slots.

---

## Classification table

| Prop | HW | Tier | Reasoning |
|---|---|---|---|
| **Scarecrow Narrator A1S** | ESP32-A1S | **2** + leader | Fleet leader / WiFi→ESP-NOW bridge (a1s doc §3). Tier-2 partner: **Skeleton A1S** — see T2-1. |
| **Skeleton Narrator A1S** (Burt) | ESP32-A1S | **2** | Speaks immediately after the Scarecrow in a tight seam. Tier-2 partner: **Scarecrow A1S** — see T2-1. |
| **Pumpkin Men** | ESP32 | **1** | **Corrected 2026-09-16 — the old CLOSE-zone jumpscare/handshake concept is dropped, no re-evaluation pending.** Same principle as Spooky Tree: latches the locked theme off the fleet broadcast and plays its own matching one of **3 flat theme tracks** (Corn/ROTLD/Mutant Pumpkins), no dynamic logic. Joins the post-Burt theme-music phase as a 5th free-running player (see [The show sequence](#the-show-sequence--one-linear-run-every-cycle)). Still **not built** — roadmap P0.8, only an 89-line bench stub exists. |
| **Spooky Tree** | ESP32 + YX5300 MP3 + HC-SR501 PIR | **1** | Self-contained for *theme* playback (own MP3 + PIR, STANDALONE + SHOW_LOCK modes). **No longer Tier 2** — the CLOSE handshake with Pumpkin Men that used to bump it up was dropped 2026-09-16 (see Pumpkin Men row). In the linear sequence it also plays the theme-music bed during the post-Burt phase. |
| **Zombie Container** | DMX spotlight + **PicoDMX (Fright Ideas)** + media player. **No ESP32 of its own** — triggered by the touchpad panel (below); `docs/zombie-container.md` | **1** | Triggered by a ~1 s relay pulse from the touchpad. The green→red→strobe→green sequence is **existing/proven** in the PicoDMX. The touchpad originates `CMD_SEGMENT_START` so Eave + the panel LED follow. **Re-check → T2-C1** only if a separate ground-breakout node must fire in step with it. |
| **Touchpad "god-mode" panel** | ESP32 + 4″ ST7796S touch (Hosyond); `props/touchpad-ota-controller/` | **1** (+ override originator) | `NODE_OTA_CONTROLLER 0x0B`. Broadcasts `CMD_OTA_MODE`, `CMD_SEGMENT_START`/`_END` (zombie), a future jukebox cue; listens for `SET_THEME`/`SHOW_STATE`. Operator-armed zombie auto-fire at the ROTLD narration start (see below). Fire-and-forget. |
| **Singing Pumpkins** (3× AtmosFX + TSP models) | ESP32 + **BrightSign XT1143** (media engine — replaces the DV68/MediaWiz, 2026-09-06) + 2 FX relays; `docs/singing-pumpkins.md` | **1** | One media player holds all the content and drives the display; the ESP32 tells it what to play (active/idle off `SHOW_STATE`; a Fisher-Yates song deck + operator jukebox for *which* song). Pumpkin-to-pumpkin sync is inside the video — never the mesh. Single node reacting to the theme cue. |
| **Stain Glass Window** | ESP32 + a media player (candidate: **BrightSign 4K1142-WW**, DE-9 serial — undecided vs a "moon/above-house" projection) + IR projector; `docs/stain-glass-window.md` | **1** | Own ESP32 + own player + own projector; plays its own SHOW/ACTION content on the theme cue. **Re-check → T2-C2** only if Joe wants the window beat-matched to a Singing-Pumpkins song as one group performance — otherwise independent. |
| **Yard Boombox 1** (Talking Skeleton) | ESP32 + YX5300 MP3 | **1** | Receive-only audio node; plays FAR/CLOSE zone tracks on the cue. Nothing depends on its exact start. |
| **Spatial audio node — patch LEFT** | Yard-Boombox-class (YX5300/SD or small A1S) | **1** | Plays the LEFT channel of the pre-mixed theme soundscape after the narrator finishes. Independent free-run off the broadcast — see [The show sequence](#the-show-sequence--one-linear-run-every-cycle). |
| **Spatial audio node — patch RIGHT** | same | **1** | RIGHT channel, same as above. The stereo image is pre-mixed + placement, never runtime sync. |
| **Crows** | ESP32 + servos | **1** | Each crow independently turns/caws toward the visitor on the cue. No crow's motion is gated on another's; slight desync between multiple crows is natural, even desirable. |
| **Area Lights (area01–06)** | ESP-01S, WS2812B rings | **1** | Receive 6-byte `CmdPacket` from a coordinator. Multiple rings, but the **coordinator owns the pattern** and near-simultaneous broadcast receipt is tighter than the eye can see on LEDs. No ring depends on another ring's state. A precise cross-ring chase = coordinator-side timed `CmdPacket`s, still Tier 1. |
| **Parkway Node 1 & 2** | ESP-01S, WS2811 | **1** | Identical firmware, deliberately share `THIS_NODE=0x07` so one Eave broadcast hits both. Two fixtures feet apart running the same effect from the same frame — sub-frame skew invisible. Neither depends on the other's state. |
| **Main Switch Controller** | ESP32 | **1** (+ fallback authority) | In normal operation: a passive listener. Only *originates* (its own PIR + `nextTheme()` + broadcasts) when its 90 s dead-man arms it as the standalone fallback (a1s doc §3.4 H4). No unicast relationship. |
| **Eave Coordinator** | ESP32 | **1** (+ lighting coordinator) | Own concern entirely — year-round calendar + 6-byte `CmdPacket` to Parkway/Area lights + `CMD_CALENDAR_REQUEST` replies. Disjoint from show `Msg` traffic (a1s doc §3.3). Sends `CmdPacket`s, needs no delivery confirmation for them. |
| **Cameras 1–3** | ESP32-S3-CAM | **1** (Cam 3) / **0** (Cam 1–2) | WiFi/HTTP. Camera 3 listens to the mesh for `SHOW_PENDING` (a listener, no coordination). Cameras 1–2 are off-mesh. |
| **Wyze Light Strip Pro** | ESP8266 / ESPHome | **0** | WiFi / MQTT only — **not on the ESP-NOW mesh.** Different transport tier. |
| **Cold Hate** | ESP32 + 7 servos | **0** | Standalone bench animatronic, no MAC/NodeID assigned, no mesh dependency. |
| **Linux Master** (detect-server) | PC | n/a — origin | Not a mesh node. Picks theme+tone, pushes it to the Scarecrow A1S over MQTT (a1s doc §3.1). |

---

## Tier 2 — the real coordination cases

### T2-1 · Scarecrow Narrator A1S ⇄ Skeleton Narrator A1S

**Why it's Tier 2:** Burt's line plays *immediately after* the Scarecrow's, as
one continuous narrator thought (`docs/_history/dialogue-architecture.md` — "cohabitation,
not overlap"). If Burt misses the cue, there's a conspicuous dead beat where the
second voice should pick up. The two figures are also physically close, so an RF
glitch that drops one unicast is plausible and worth a couple of retries.

**Mechanism:** fully specified in `docs/a1s-board-and-audio-architecture.md`
§3.4 **H1b**. Summary:
- **Cue (Scarecrow → Burt):** on each narrator cue, after the normal fleet
  broadcast, unicast `CMD_NARRATOR_2_START` (same `session_id`, `payload_a=theme`,
  `payload_b=tone`) to `MAC_SKELETON_A1S`. Native send-callback → `SUCCESS` =
  done; `FAIL`/timeout → retry, **≤3 total, ~20–50 ms apart**; after 3, proceed
  regardless (Burt's own §2.3 fallback ladder covers a true miss).
- **Handoff (Burt → Scarecrow):** when Burt starts speaking he unicasts a
  `CMD_ACK` ("starting", `payload_a=NODE_SKELETON_A1S`, `payload_b=session_id`)
  back to `MAC_SCARECROW_A1S`, same ≤3-try non-blocking pattern.
  **Informational/diagnostic only** — the show never waits on it.
- **Logging:** both directions record `success|fail` + attempt count per cue, so
  a flaky Burt node shows up in post-show review before it becomes a visible
  failure.
- **Not built:** custom ACK packets, MAC-pairing ceremony, blocking waits.

> **Superseded 2026-09-16:** this section used to specify a T2-2 "Pumpkin Men
> ⇄ Spooky Tree two-phase CLOSE handshake," ported from the 2025
> `Projects/Master/src/zones.cpp` proximity-zone jumpscare
> (`mp3WarmHandshakeBlocking()`, `gCloseAwaitingP2`, `closePartBStart()`). Joe
> confirmed the CLOSE-zone scare concept is **gone** — it belonged to the old
> FAR/MID/CLOSE proximity model, never reconciled with the 2026-09-06 linear-
> sequence rewrite below. Pumpkin Men is now Tier 1: same three-flat-theme-
> track principle as Spooky Tree, no cross-unit gate, no unicast handshake. See
> the Pumpkin Men row in the classification table above and
> [The show sequence](#the-show-sequence--one-linear-run-every-cycle).

---

## Tier 2 — candidates, NOT upgraded (need a specific reason from Joe first)

### T2-C1 · Zombie Container ⇄ ground-breakout / second container

Today: **one** container-controller node in the inventory; its flame, air-blast,
and breakout motion are one board's local sequence → **Tier 1**. The dialogue
geography (`docs/_history/dialogue-architecture.md`) also mentions "a second zombie container
running a looped video" and "a zombie breaking out of the ground" — if either
becomes **its own mesh node** *and* must fire in a timed sequence with the main
container (e.g. ground-zombie erupts exactly as the main container's air-blast
hits), that pairing is a real Tier-2 case. **Re-evaluate when those units are
actually built and their triggering is specified** — not before.

### T2-C2 · Stain Glass Window ⇄ Singing Pumpkins

Both sit in the "Mutant Pumpkins" cluster. If Joe wants the stained-glass window
animation **beat-matched** to a Singing-Pumpkins song as a single group
performance, the window's start must be gated on the pumpkin sequence → Tier 2
(Singing Pumpkins → Stain Glass "song N starting now"). If the window just runs
its own atmospheric loop during the theme (the current assumption), it stays
**Tier 1**. **Decision belongs to Joe's show design, not this doc.**

---

## The show sequence — one linear run, every cycle

**Corrected 2026-09-06 (supersedes the earlier "two trigger points" /
concurrent-start model, which was overcomplicated).** Every cycle is the **same
strict linear sequence**, no variation:

1. **Scarecrow narrates.** Always first.
2. **Burt narrates.** Always second, immediately after. No variation.
3. **Creepy-theme-music phase.** After Burt: **no more narration, period.** The
   locked theme's ambient/music audio plays **and** Singing Pumpkins runs its
   performance — **both start together, on the same trigger** (`CMD_NARRATOR_2_DONE`
   / the leader's post-narrator cue), **regardless of which theme won**. This
   phase runs to the cycle reset.

The theme's ambient/music in step 3 is delivered by the **Spooky Tree + the two
spatial audio nodes (patch L/R) + Pumpkin Men** — each playing its own
SD-resident, pre-mixed, theme-matched track. With Singing Pumpkins that's
**five independent players, one trigger, free-running:**

| Player | Physical role | Content (per locked theme) |
|---|---|---|
| **Spooky Tree** | centre / the tree | theme ambient/music bed (`PLAY_AMBIENT` / theme tracks 7/8/9) |
| **Spatial audio — patch LEFT** | left of the patch | left channel of the pre-mixed theme soundscape |
| **Spatial audio — patch RIGHT** | right of the patch | right channel of the pre-mixed theme soundscape |
| **Pumpkin Men** | jaw relays / fog / skeleton prop | its own matching one of 3 flat theme tracks — Corn / ROTLD / Mutant Pumpkins |
| **Singing Pumpkins** | the pumpkin display | its performance for the active theme |

**Why still Tier 1 — no lockstep sync:**
- Each track is **self-contained and pre-mixed**. The stereo / surround image is
  physical speaker placement + how the tracks were mixed ahead of time — **not**
  runtime coordination.
- All five start off the **same broadcast** and then **free-run**. Tens of ms of
  start-skew is inaudible for ambient/music beds (no sharp transients to phase).
- No player depends on another player's *state*. One missing → the others still
  play, the bed just loses a component. Graceful degrade = the mesh contract.
- Singing Pumpkins' 3+ pumpkins sync **with each other** inside the media
  player's video (one BrightSign XT1143, one display), never the mesh (see the
  classification table).

> Provisioning: the two spatial nodes are Yard-Boombox-class players (YX5300/SD
> or a small A1S) — one track per theme per side, selected by the latched
> `(theme)`. On A1S hardware they can reuse the narrator SD layout
> (`docs/a1s-board-and-audio-architecture.md` §2.1).

### Zombie Container breakout — operator-armed, conditional on the ROTLD theme

**Updated 2026-09-06 (supersedes the earlier "purely unscheduled / ad-hoc" model).**
The breakout is still **optional** — the operator **arms or disarms** it (touch
panel / manual). But **when armed, it is deterministic, not random:** it fires at
the **start of the *Return of the Living Dead* (ROTLD) narration**, cued off the
Scarecrow A1S preshow burst (`CMD_SET_THEME=ROTLD` + `CMD_SHOW_STATE(SHOW_NARRATOR)`).
So it fires every ROTLD cycle while armed, and never on the other two themes.
Manual "FIRE NOW" stays available any time. Full design: `docs/zombie-container.md`.

Mesh-wise the **touchpad panel** (`NODE_OTA_CONTROLLER`) pulses a relay (~1 s)
into the **PicoDMX** and broadcasts `CMD_SEGMENT_START`. The **Eave Coordinator**
and the **panel's own RGB LED** each run an independent, self-timed
**green → red → strobe → green** state machine off that one broadcast (shared
`Z_*_MS` / `DMX_*_MS` phase constants — Eave's `fw 2026-09-01-zombie1` implements
it as `Z_RED/GREEN/WHITE`, to be reconciled with the no-green-hold version).
Self-timed, not frame-locked — "doesn't have to match the strobe perfectly".
Whether the phase *timing* lives in the PicoDMX scene or the ESP32 manages it is
an **open question** — `docs/zombie-container.md`. `CMD_SEGMENT_END` drops
everyone back to ambient. (T2-C1 below still applies only if a separate
ground-breakout unit must fire *in step* with it.)

### Pumpkin Men content model — the simplest node in the fleet

**Corrected 2026-09-16 — no handshake, no proximity zone.** Pumpkin Men follows
the exact same principle as Spooky Tree: **three flat theme tracks** — Corn /
ROTLD / Mutant Pumpkins — **no dynamic logic**. Latches `(theme)` off the fleet
broadcast, plays the matching track, done. No tone branching, no per-visitor
variation, no pool, no gate on any other unit. It joins the post-Burt
theme-music phase as a free-running Tier-1 player (see the table above).
**Firmware written 2026-09-16** — `props/pumpkin-men/` (roadmap P0.8), builds
clean, not yet flashed/bench-tested. 2025 reference code: `Projects/Master/`.

---

## Forward-looking notes (concept only — not built)

### FL-1 · Singing Pumpkins — BrightSign taunt/idle mode (fixed-duration reset)

**Captured 2026-09-06; scaffolded `props/singing-pumpkins/` same day; hardware
confirmed 2026-09-06.** Singing Pumpkins' media engine becomes a **BrightSign
XT1143** — this **replaces the DV68 / MediaWiz Sprite** (not an add-on). Joe has
2× XT1143 + 1× BrightSign 4K1142-WW, all replacing the 2024 MediaWiz players.
The ESP32 drives the XT1143 over its **3.5 mm TRS serial jack** (Tip=RX, Ring=TX,
Sleeve=GND, 115200 8N1) via a MAX3232 3.5 mm adapter. It's a **new serial
integration**, not a reuse of the (IR) projector path. Content: a Fisher-Yates
song-shuffle deck ("let them sing") + an operator **jukebox** pick from a touch
pad (location undecided). Full design + open items: `docs/singing-pumpkins.md`.

**Two modes, chosen locally off one broadcast signal — no presence detection:**
- **Active-show mode** — taunting / performance content, while `SHOW_STATE` is
  a non-idle value.
- **Idle-ambient mode** — random selection from an ambient-music library, when
  `SHOW_STATE` returns to idle/off.

**How the switch happens (deterministic):** the creepy-theme-music phase (Singing
Pumpkins + Spooky Tree + the two spatial audio nodes) starts on
`CMD_NARRATOR_2_DONE` and runs for a **fixed duration** (`SHOW_ACTIVE_MS`), then
the cycle **resets** — the leader drops `SHOW_STATE` back to idle/off, freeing
itself for Frigate's next trigger. Singing Pumpkins just watches that
`CMD_SHOW_STATE`:

- non-idle (`SHOW_ACTIVE` / `SHOW_NARRATOR` / `SHOW_PENDING`) → active-show
  content;
- `SHOW_IDLE` / `SHOW_OFF` → idle-ambient (random ambient-music library).

**No presence interpretation, no debounce, no "guests cleared out" logic
anywhere.** One deterministic reset signal on a fixed timer the leader owns.
Singing Pumpkins stays a **passive Tier-1 listener** — no cross-node dependency.

Fallback if the node hears **no** `CMD_SHOW_STATE` for a long window (leader
down / off-season): default to idle-ambient.

Not urgent — downstream of the Singing Pumpkins node being built at all
(`inventory-report.md`: PENDING, 2025 code only). The `SHOW_ACTIVE_MS` value it
depends on is discussed in "Cycle timing" below.

---

## Cycle timing — simple sequential addition

**Corrected 2026-09-06.** With the linear sequence (Scarecrow → Burt → theme
audio + pumpkins), there are **no overlapping clocks to reconcile**. Total cycle
= straight sum:

```
  Scarecrow monologue
+ Burt monologue
+ creepy-theme-music phase  (Spooky Tree + spatial L/R + Singing Pumpkins,
                             all start together, run to reset)
= total cycle
```

The Zombie Container breakout is **not in this math** — when armed it fires at the
ROTLD narration start and the PicoDMX runs its own timeline in parallel; it does
not extend the cycle length (see "Zombie Container breakout" above).

**The mechanism.** `main_switch_controller/src/main.cpp:216-218`: on
`CMD_NARRATOR_2_DONE` → `setShowState(SHOW_ACTIVE)` and
`g_showActiveUntilMs = now + SHOW_ACTIVE_MS`; on timeout →
`SHOW_ACTIVE → SHOW_OFF (2 s) → SHOW_IDLE` (the reset). The creepy-theme-music
phase = this `SHOW_ACTIVE` window.

### `SHOW_ACTIVE_MS` = **210 000 ms** — CONFIRMED 2026-09-06

Joe: **one standard show is 3 min 30 s** (there are three theme shows). So the
active window is **210 s**, now a **named shared constant**
(`SHOW_ACTIVE_MS` in `shared/Shared_Constants/ProjectConstants.h`).

- **Single fixed value, not theme-dependent** — one deterministic reset for the
  whole cluster.
- **The theme-music tracks must fit inside 210 s** (or loop / be cut at the
  reset). Spooky Tree beds + Pumpkin Men's tracks + the Singing Pumpkins content
  should be authored to ≤ ~210 s; patch L/R beds loop. The old "must be ≥ the longest track, and none
  are measured" framing is replaced — the **show length drives the number**, and
  the content is cut to fit it.
- **Still to fix in code** (both un-migrated 2025 tree):
  `main_switch_controller/src/main.cpp:218` hardcoded `180000` →
  `SHOW_ACTIVE_MS`; `showsim/constants.py` has no main-show duration constant —
  add `SHOW_ACTIVE_MS = 210_000`.
- **Cycle END/reset-signal owner** — still deliberately open per Joe.

---

## Summary

- **Tier 2 (real, specified):** Scarecrow A1S ⇄ Skeleton A1S — see T2-1.
  (Pumpkin Men ⇄ Spooky Tree's old CLOSE handshake was dropped 2026-09-16 —
  both are Tier 1 now.)
- **Tier 2 (candidate, needs Joe):** Zombie Container multi-unit; Stain Glass ⇄
  Singing Pumpkins.
- **Everything else: Tier 1** — broadcast-only, react independently. The show is
  one **linear sequence** (Scarecrow → Burt → creepy-theme-music phase). The
  post-Burt phase = Spooky Tree + 2 spatial patch nodes + Pumpkin Men + Singing
  Pumpkins, all starting on one trigger and free-running, no sync. The
  **Zombie Container breakout is an unscheduled optional highlight**, not a
  timed beat.
- **Tier 0 (off show mesh):** Wyze strip (MQTT), Cold Hate (standalone),
  Cameras 1–2 (WiFi/HTTP).

## Timing status

- Total cycle = simple sequential sum: Scarecrow monologue + Burt monologue +
  creepy-theme-music phase. No overlapping clocks.
- **`SHOW_ACTIVE_MS` = 210 s — CONFIRMED 2026-09-06** (one standard show = 3 min
  30 s; three theme shows). Now a named shared constant in
  `ProjectConstants.h`. Theme-music content is authored to fit inside it (loop or
  cut at the reset), not the other way round. **Still to fix:**
  `main_switch_controller/src/main.cpp:218` (`180000` literal) and
  `showsim/constants.py` (no constant yet).
- **Open for Joe:** cycle END/reset-signal owner (deferred).

The pattern is always the same where Tier 2 applies: **fleet broadcast
unchanged**, plus a targeted unicast on the **native** ESP-NOW delivery result,
**≤3 closely-spaced tries, then proceed regardless, never blocking**, both
directions logged. No custom ACK protocol, no pairing ceremony, no wait loop.

## References

- `docs/a1s-board-and-audio-architecture.md` §3.4 H1 / H1b — the broadcast rule
  and the Scarecrow↔Burt native-unicast handshake this table generalises
- `docs/_history/dialogue-architecture.md` — narrator seam ("cohabitation, not overlap")
- `docs/inventory-report.md` §3 (prop inventory), §5 (inter-prop dependency map)
- `props/spooky-tree/` — the one confirmed-operational node; `IMPROVEMENTS.md`
  1.5 (CMD_TRIGGER overload) is relevant if a `CMD_CLOSE_PHASE` is added
- `~/Documents/Halloween-Dev/PlatformIO/Projects/Master/src/zones.cpp` — the
  2025 two-phase CLOSE handshake to port
- `~/Documents/Halloween-Dev/PlatformIO/Projects/Singing pumpkins V2/` — proof
  the 3 pumpkins run off one DV68 / one ESP32 (Tier 1)
- `shared/Shared_Constants/ProjectConstants.h` — `Msg`, `Command`, MACs,
  `nodeIdToMac()`
