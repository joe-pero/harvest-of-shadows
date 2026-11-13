# spooky_tree — Improvement Review

Reviewed 2026-08-28. Scope: `src/main.cpp` (364 L), `espnow_handler.cpp/.h`,
`mp3_player.cpp/.h`, `pir_sensor.cpp/.h`, `platformio.ini`.
Role: Authority→Tree. **The one node confirmed operational on real hardware
(2026-05-27, stable 20+ min).** This review is about the gaps that only show up
under show conditions (SHOW_LOCK, rapid commands, long runs) — bench-stable
doesn't mean show-ready.

---

## Tier 1 — show-condition reliability

### 1.1 The MP3 command handlers block the loop, so SHOW_LOCK loses 2 of its 3 frames
**FIXED 2026-09-04 — the tree's half.** Both prescribed halves applied:
- `espnow_handler.cpp` now has an 8-slot ring buffer (`s_queue`/`s_qHead`/`s_qTail`)
  instead of the single-slot `s_pendingMsg`. `espnowTick()` drains everything
  queued in one pass instead of one message per call.
- `onEspNowCmd()` no longer calls `mp3Stop()`/`mp3PlayTrack()`/`mp3Duck()` inline.
  It sets `g_pendingMp3Op` (+ `g_pendingMp3Track`); `loop()` applies it once,
  right after `espnowTick()` returns — so the queue drain itself is never
  blocked by a 250 ms YX5300 pace delay. Build verified green.

**Moot 2026-09-24: Main Switch retired (Joe); the tree no longer handles SHOW_LOCK.** Previously still open: this only fixes the tree's side. `main_switch_controller`'s
`broadcastShowLock()` still sends `CMD_SHOW_LOCK` + `CMD_SET_THEME` +
`CMD_SET_MODE` back-to-back — that's fine now (the tree's queue absorbs the
burst instead of losing frames to overwrite), but the underlying spacing issue
in IMPROVEMENTS 0.2/0.3 over there is still worth doing for its own sake, and
no other node's inbound path has been audited for the same single-slot pattern.
Not live-tested against real SHOW_LOCK traffic yet — bench/compile-verified only.

### 1.2 The ambient heartbeat defeats the stall detector
**FIXED 2026-09-24.** The 30 s `mp3StampActivity()` hack is gone. The shared
SafeYX5300 driver polls `queryStatus` every 15 s, and "activity" is now the last
byte actually received from the module, so a module that stops answering is
detected (driver re-handshakes at 60 s; main.cpp restarts ambient at 7 min and
reboots 2 min later). A module that goes quiet while still "alive" is caught by
the poll seeing `stopped` while a track should be playing.
`main.cpp:317-328` stamps `mp3StampActivity()` every 30 s while
`g_playState == PLAY_AMBIENT`, unconditionally. The stall detector
(`main.cpp:300`) fires when `(now - lastAct) >= 7 min`. So if the YX5300
**silently stops producing audio but the ESP loop keeps running** — the exact
failure the recovery code exists for — `lastAct` keeps getting refreshed every
30 s and the stall path **never triggers**. The comment even says it: "this
proves the loop is alive" — but loop-alive was never the risk; the TWDT already
covers that.

Fix: get a *real* audio-liveness signal. MD_YX5300 has `queryStatus()` /
`playStatus()`. Every ~45–60 s during playback, send a status query; stamp
`lastAct` only when a reply comes back, and if the reply says "stopped" while
you believe you're in `PLAY_AMBIENT`/`PLAY_ACTION`, restart the track. Drop the
blind 30 s heartbeat. This turns the stall subsystem from decorative into real.

### 1.3 Theme tracks (7/8/9) can hang SHOW_LOCK forever
**FIXED 2026-09-24.** SHOW_LOCK is gone (Main Switch retired). Theme runs are now
driven by the Scarecrow via `ShowFollower.h`, and the shared SafeYX5300 driver STOPs
every theme track at its known length (3:00 + 2 s), with a 230 s follower fail-safe
behind it. See `docs/show-follower.md`, `docs/mp3-yx5300-driver.md`.
- Per the handoff doc, tracks 7/8/9 (`TRACK_THEME_CORN/ROTLD/MPUMP`) are
  **"NOT confirmed on SD"**.
- `mp3PlayTrack()` only arms the missed-EOT deadline for tracks **2–6**
  (`main.cpp` in `mp3_player.cpp:94`). Theme tracks get `s_trackDeadlineMs = 0`.
- So if track 7 doesn't exist, or `STS_FILE_END` is missed (the whole reason
  deadlines exist), `g_playState` stays `PLAY_THEME` **indefinitely** and the
  tree silently sits out the rest of the show.

Fix: (a) confirm 7/8/9 are actually on the card and log their real durations;
(b) arm a deadline for theme tracks too — `max(known_duration * 1.5, 90 s)`.

### 1.4 Verify action-track lengths against the 60 s deadline
`MP3_TRACK_DEADLINE_MS = 60000` is applied to tracks 2–6. `main.cpp:66`'s
comment says "longest action track is 6:39". **If any action track really runs
longer than 60 s, the deadline force-ends it at 0:60** — `mp3Tick()` sets
`s_trackFinished`, the loop returns to ambient and sends `TREE_MOTION_END`
mid-scare. Either the tracks are all ≤60 s (then fix the misleading comment) or
the deadline needs to be per-track from a duration table (like Talking Skeleton
/ Stain Glass do). Confirm on the real SD card.

### 1.5 `CMD_TRIGGER` is overloaded — show-fire vs. tree-motion event
**FIXED 2026-09-24.** The tree no longer receives or sends `CMD_TRIGGER`. PIR motion
is its own `CMD_PROP_MOTION` (0x47, payload_a = NODE_SPOOKY_TREE), sent 3x.
The tree **receives** `CMD_TRIGGER` to mean "play the theme track" (SHOW_LOCK),
and **sends** `CMD_TRIGGER` (broadcast, `payload_a = NODE_SPOOKY_TREE`,
`payload_b = 1/0`) to announce its own PIR motion in STANDALONE
(`espnowSendTreeMotionStart/End`). Any other node that acts on a received
`CMD_TRIGGER` without checking `payload_a`/`sender` will treat the tree's
motion broadcast as a show trigger and fire spuriously. There's no dedicated
sensor/motion command in the 2026 vocabulary.

Fix: add `CMD_TREE_MOTION` (or a general `CMD_SENSOR_EVENT`) and use it for the
STANDALONE broadcast. Audit every execution node's `CMD_TRIGGER` handling
during the fleet review. (Flagged in the progress tracker as a cross-node item.)

---

## Tier 2 — correctness / robustness

### 2.1 `CMD_SET_MODE` is silently ignored
`main_switch_controller` broadcasts `CMD_SET_MODE` (friendly/scary) as part of
`broadcastShowLock()`. `onEspNowCmd` has no case for it → `default: break`. If
friendly vs. scary should change anything on the tree (volume, a tamer theme
track, skipping the elk scream), that's unimplemented. If it genuinely doesn't
matter for this node, add a `// CMD_SET_MODE: intentionally ignored — tree
content is theme-only` comment so it reads as deliberate.

### 2.2 Fixed 5 s duck vs. variable-length narration
`mp3Duck()` restores volume after `MP3_DUCK_MS = 5000`. Narrator lines are
0.5–15 s (per `show_behavior.md`). Unless `main_switch`/the A1S nodes re-send
`CMD_DUCK` every <5 s for the duration of a line, the ambient bed pops back to
full volume under the narrator. Better: `CMD_DUCK` carries a duration in
`payload_a`, or an explicit `CMD_UNDUCK` brackets it.

### 2.3 `while (!Serial) {}` in `setup()` (`main.cpp:212`)
No-op on this WROOM, but an instant headless brick if ever built for an S3/C3.
`while (!Serial && millis() < 2000) {}`.

### 2.4 `pickActionTrack()` anti-repeat is a no-op for adjacent duplicates
Deck `{2,2,3,4,5,6}`. When `s_actionDeck[s_deckPos] == s_lastActionTrack` it
swaps with `s_deckPos + 1` — but if the deck is `[..., 2, 2, ...]` and last was
2, the swap produces `[..., 2, 2, ...]` again and you still return 2 (immediate
repeat) — exactly the case the guard is for. Scan forward for the first
non-matching card instead of a single swap.

### 2.5 `verifyNodeMac()` infinite halt kills OTA recovery
Documented behavior ("halts + blinks GPIO2"), and OTA isn't always-on here so
it's less severe than on `main_switch`. Still — consider halting *soft* (refuse
to act on commands, blink GPIO2 from `loop()`) so a `CMD_OTA_MODE` can still be
honored to push a corrected build. Otherwise wrong-board = a ladder and a USB
cable.

### 2.6 `g_bootCount` is collected but unused
`RTC_DATA_ATTR g_bootCount` increments every boot and is only printed. Put it in
the `CMD_NODE_ONLINE` payload so the show controller can see a node that's
power-cycling (a fast-rising boot count on the tree = the exact "power issue on
one unit" symptom that hit parkway unit 1). Reset it after N minutes of clean
uptime.

### 2.7 PIR input has no failsafe pull
`pinMode(PIR_PIN, INPUT)` — fine while the HC-SR501 is connected (push-pull
output), but a broken signal wire leaves GPIO27 floating → random triggers all
night. `INPUT_PULLDOWN` fails safe to "no motion".

### 2.8 False STALL DETECTED on every PLAY_IDLE → PLAY_AMBIENT entry
**FOUND + FIXED 2026-09-04**, live on hardware during the post-reflash bring-up
(this is what a real reboot immediately surfaced that compile-only review
hadn't). `loop()` captures `now` once at the top, then the `PLAY_IDLE` branch
calls `mp3PlayTrack(TRACK_AMBIENT)` — which sets `lastAct = millis()` to a
*later* timestamp than `now`, since it runs partway through the same
iteration — and fell through into the stall-check block below without
returning. `now - lastAct` (both `uint32_t`) underflows to just under
`UINT32_MAX`, which is trivially `>= MP3_STALL_THRESHOLD_MS`, so
"STALL DETECTED — restarting ambient" fired immediately on **every single
boot and every SHOW_UNLOCK** — confirmed on the actual board (`[mp3] Playing
track 1` immediately followed by the false stall message and a second replay
from 0:00). The EOT-restart branch a few lines below already guards against
this exact hazard with `return; // skip stall check this iteration`; the
`PLAY_IDLE` entry path just never got the same guard. Added the matching
`return;`. Verified via reflash — ambient now starts clean, no double-restart.

---

## Tier 3 — lower priority

- ~~**`esp_wifi_set_ps(WIFI_PS_NONE)`**~~ **FIXED 2026-09-04.** Turned out not
  hypothetical — `otaServiceStart()` already calls `WiFi.begin()` for the
  CMD_OTA_MODE window, so modem-sleep jitter was a live risk during every OTA
  push, not just a future one. Added `WiFi.setSleep(false)` +
  `esp_wifi_set_ps(WIFI_PS_NONE)` in both `espnowInit()` (espnow_handler.cpp)
  and `otaServiceStart()` (main.cpp), matching `eave_coordinator`'s pattern
  (`docs/espnow_audit.md` §4.1). Build verified green.
- ~~**Local `platformio.ini`** — pin `platform = espressif32@7.0.1`~~ **FIXED
  2026-09-04**, done as part of the harvest-of-shadows migration.
- **`main.cpp:66` comment** — "longest action track is 6:39" as the rationale
  for a 7-min *ambient* stall threshold is confusing; ambient is ~6:21 and the
  stall detector doesn't run during `PLAY_ACTION`. Reword.
- **Track-map duplication** — `mp3_player.h` defines both `TRACK_CORN_MIN/MAX`
  (3–9, unused) and `TRACK_THEME_CORN` (7). Pick one representation; the unused
  range macros invite confusion with the handoff doc's "Corn (3–9)" banks.

---

## Tier 4 — testing

Add a `native` test env for the pure logic:
- `pickActionTrack()` / `shuffleActionDeck()` — weighting (track 2 ≈ 2×
  frequency), no adjacent repeats over 10 000 draws, deck refills.
- `msg_seal`/`msg_valid` round-trip + bit-flip rejection.
- The play-state transitions given a scripted `Msg` sequence, including the
  1.3 theme-hang and the 1.1 frame-loss scenarios (which a test would have
  caught immediately).

---

## Quick-win checklist
- [x] **1.1** Ring-buffer inbound ESP-NOW; move `mp3*` calls out of the dispatch — 2026-09-04, tree side only; board itself live-verified (reflash, boot, OTA all confirmed on hardware), but no real multi-frame SHOW_LOCK burst tested yet — no ESP-NOW sender board on hand this session
- [x] **2.8** False STALL DETECTED on every ambient (re)start — 2026-09-04, live-verified
- [ ] **1.2** Replace the blind 30 s heartbeat with a real `queryStatus()` poll
- [ ] **1.3** Confirm tracks 7/8/9 exist; arm a deadline for theme tracks
- [ ] **1.4** Confirm action-track durations vs the 60 s deadline
- [ ] **1.5** Add `CMD_TREE_MOTION`; stop overloading `CMD_TRIGGER`
- [ ] **2.1** Handle or explicitly no-op `CMD_SET_MODE`
- [ ] **2.2** Variable-length duck
- [ ] **2.6** Boot count → `NODE_ONLINE` payload
