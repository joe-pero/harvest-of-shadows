# Show follower: how theme-bed props follow the Scarecrow

2026-09-24. Code: `shared/Shared_Constants/ShowFollower.h`. Used by
`props/spooky-tree` and `props/pumpkin-men`. Host test: `bash props/_tests/run.sh`.
Timing and audio side: `~/Documents/show-sync-orchestrator/`.

**The Scarecrow is the show controller. Its theme is the theme.** Props never
decide the theme or the show timing themselves. They follow the cues, and each
safeguard below is there so a lost, repeated, stale or foreign frame can't cause
a misfire.

## What the Scarecrow sends (commit 711f9e2, all 3x bursts)

| When | Frames (same `session_id`) |
|---|---|
| PRE-SHOW | `SET_THEME(theme)`, `SET_MODE(tone)`, `SHOW_STATE(SHOW_NARRATOR)` |
| MAIN-SHOW | `START_SCENE(theme)` … `SHOW_STATE(SHOW_ACTIVE)` |
| +210 s (`SHOW_ACTIVE_MS`) | `SHOW_STATE(SHOW_OFF)`, 2 s, `SHOW_STATE(SHOW_IDLE)` |
| every 10 s | `CMD_HEARTBEAT` (liveness only) |

A new PRE-SHOW during an open window closes the old window without sending OFF.
The new `SHOW_NARRATOR` is the signal. Old Scarecrow firmware (still on the board
until it's reflashed) sends `SHOW_IDLE` as its 10 s heartbeat instead. The rules
below are safe with both.

## What a follower does

| Event | Result |
|---|---|
| `SET_THEME` / `START_SCENE` from the Scarecrow | Theme latched. If it changes the theme of the run that's playing, the track is **replaced**. |
| `SHOW_NARRATOR`, new session | New visit. A previous visit's theme track still playing is **stopped** and the prop goes back to its idle mode under the narrators. Tree: ambient continues, PIR action tracks held off. |
| `SHOW_ACTIVE` | Latched theme track starts, **replacing whatever is playing**. |
| same-session `SHOW_ACTIVE` repeat < 10 s | Burst duplicate, ignored (even after a STOP). |
| end-of-track < 3 s after PLAY | Stale event from the replaced track, ignored. |
| end-of-track | Run done, back to idle mode. WARN if the length is more than 10 s off the expected 3:00 (wrong file on the SD card?). |
| `SHOW_OFF` / `SHOW_IDLE` | Ends the run **only after 3:00 of playing** (so the old-firmware heartbeat can't cut it). |
| nothing ends the run | **Fail-safe at 230 s** (`SHOW_ACTIVE_MS` + 20 s): back to idle mode, WARN. |
| show cue from any other sender | Ignored, counted (`badSender`). |
| invalid theme value | Ignored, theme unchanged. |
| `SHOW_ACTIVE` with no theme for that session | **No guess** (2026-10-26): a stale track stops, the prop sits this show out in its idle mode, WARN (`unconf`). Silence beats the wrong theme. A walkway-only main show carries its theme in `START_SCENE` alone. |
| no Scarecrow frames for 35 s | WARN "Scarecrow silent". The track keeps playing; the 230 s fail-safe bounds it. |
| visit that never reaches `SHOW_ACTIVE` | Times out after 10 min (bookkeeping only). |
| MP3 start not confirmed / file missing | Run ends at once, back to idle mode, WARN (`playFail`). No silent wait for the fail-safe. |
| `CMD_STOP` / bench `stop` | Hard stop, follower forgets the run. |

Every decision prints a `[show]` line. The 60 s alive print adds a `[show]`
status line with counters: `plays repl dup earlyEOT earlyOFF deadline unconf lenWarn badSender`.

**Main Switch Controller retired (Joe, 2026-09-24).** Neither node handles
`SHOW_LOCK` / `SHOW_UNLOCK` / `TRIGGER` / `DUCK` any more; only the Scarecrow's
cues, `CMD_STOP` and `CMD_OTA_MODE`.

The MP3 side (confirmed start, stop at known length, verified stop, defuse,
module recovery) is `docs/mp3-yx5300-driver.md`.

## Bench (USB serial, e.g. through the yard-bridge-pi Pi)

| Command | Acts as |
|---|---|
| `visit corn\|rotld\|mutant` | new session: `SET_THEME` + `SHOW_NARRATOR` |
| `corn` / `rotld` / `mutant` | `SET_THEME` (same session) |
| `active` | `SHOW_ACTIVE` |
| `off` / `idle` | `SHOW_OFF` / `SHOW_IDLE` |
| `hb` | `CMD_HEARTBEAT` |
| `fake <cmd>` | the same frame from a non-Scarecrow sender (must be ignored) |
| `show` | follower status |
| `mp3` | MP3 driver status + counters |

These go through the real `onEspNowCmd()`, the same path a radio frame takes.
The bench session ID only changes on `visit`.

## Tree motion → `CMD_PROP_MOTION`

Spooky Tree's PIR used to broadcast `CMD_TRIGGER`, which meant "start the show"
on the Main Switch path and could start Pumpkin Men. It now sends its own
`CMD_PROP_MOTION` (0x47, `payload_a` = `NODE_SPOOKY_TREE`, `payload_b` = 1),
one frame 3x. Nothing acts on it yet; it's for the Scarecrow to react to later
(narrator change, Joe's call). The tree reports motion even while the narrators
talk, but only plays its action track when no visit is in progress.

## 2026-09-26: signed elapsed (`13d54c8`)

The nodes read `now` once at the top of `loop()`. Frames handled after that stamp
a *later* `millis()`, so unsigned `now - visitStart_` wrapped to about 49 days.
The result: "visit never reached SHOW_ACTIVE, timed out" the instant a visit
started, so the narrators-talking PIR hold never applied. The same wrap could hit
the R7 fail-safe right after a theme started. `ShowFollower` now uses a signed
`since()`, and Pumpkin Men + Spooky Tree pass a fresh `millis()` to
`tick`/`onTrackEnd`/`onPlayFailed`. Host tests ALL PASS. Checked against the
Scarecrow's real sends (read only): theme burst SET_THEME + SET_MODE +
SHOW_NARRATOR ×3, SHOW_ACTIVE ×3, reset = SHOW_OFF then SHOW_IDLE 2 s later,
CMD_HEARTBEAT liveness. Both nodes' ESP-NOW RX queue went from 8 to 32 slots,
since one burst is 9 frames (`1e3050c`). Pumpkin Men's Mutant theme plays 004 (2:09): an
early SHOW_OFF can't cut it (offMinRunMs 180 s), and the track ending on its own
returns to idle normally.


## Known issue + decisions (2026-10-26, live desync)

**Desync seen:** ~19:26 the Tree dropped to ambient while Pumpkin Men + the door kept playing Mutant Pumpkins.

**Cause (Tree black box, `logs/tree/tree-20261026-195638-tail.log`, `logs/tree/observations.md`):**
1. 19:25:58 walkway-only main show: Tree got START_SCENE(MUTANT) + SHOW_ACTIVE, track 9 confirmed. Theme logic was right (`unconf=0` all night).
2. The encore gets its OWN session from the bridge (no START_SCENE, no SHOW_ACTIVE, banter only).
3. The Scarecrow answers any `CMD_NODE_ONLINE` (Pumpkin Men pings every 5 min) with a full theme burst for its current session: `SET_THEME + SET_MODE + SHOW_STATE(SHOW_NARRATOR)` (`props/scarecrow-narrator-a1s/src/main.cpp`, loop: "answering CMD_NODE_ONLINE with a fresh theme burst").
4. That NARRATOR carried the encore's new session, so the Tree's R3 ("new visit -> stop the previous theme") stopped Mutant ~60 s in. Same R3 cut ROTLD 3 s in at 19:20:02 (a pre-show that was then abandoned).
A resync burst and a real pre-show are identical on the wire; a follower can't tell them apart.

**Fix FLASHED 2026-10-27 on all four (same follower code as `3879b41`): Patch Right `3879b41`, Tree `d5ae582`, Patch Left `6c6c608`, Pumpkin Men `b7ef5fc` (+ fog cooldown). Bench PASS on Tree, Patch L/R (fake new visit 60 s into a theme -> `new visit while playing (kept playing)`, theme ends at 3:00); Pumpkin Men bench pending main power. Live proof = next real show: pull logs, check no theme cut. Was planned as:** R3 must not cut a playing theme: set `Config::stopOnNewNarrator = false` in every follower (Tree, Pumpkin Men, Patch L/R). A playing theme then ends at its own 3:00, or is replaced by the next real SHOW_ACTIVE (R4). Trade-off: a new group's narrators may start over the tail of the old theme. Scarecrow stays untouched (Joe).

**Built `1fa5794` (not flashed) — R4 no-guess:** SHOW_ACTIVE plays only a theme latched for THAT session; otherwise a stale track stops and the node sits the show out (WARN, `unconf`). Harmless, but not the 10-26 cause.

**Rejected:** stopping a run when the Scarecrow heartbeat's session moves on — encores get a new session with no music, so it would cut every theme during an encore (test `encore_session_keeps_theme`).

**Design facts to remember:** ambient beds are free-running per node (never synced); only theme tracks start together on SHOW_ACTIVE. Opening the bridge port did NOT reboot the Tree on 10-26 19:56 (shows as a ~36 s loop block).
