# SafeYX5300: the shared hardened MP3 driver

2026-09-24. Code: `shared/SafeYX5300/SafeYX5300.{h,cpp}`. Used by `props/spooky-tree`
and `props/pumpkin-men` through their thin `mp3_player.{h,cpp}` wrappers. The Patch
L/R nodes should use it when they're built.

**Built from Joe's own 2025 code and notes, not the datasheet:**
- `Halloween-Dev/PlatformIO/Master Halloween Show 2026/docs/harvest_notes/mp3_serial_findings.md` and `hardware_quirks.md`
- `Projects/Scarecrow/src/Scarecrow.ino` (`mp3StartReliable`, `safeStop`, the idle defuse, `drainUART`)
- `Projects/Scarecrow/src/yx5300_player.cpp`, `Projects/Talking Skeleton/src/ts_mp3.cpp`, `Projects/Master` (`zones.cpp`)

## Rules

| # | Rule | Why |
|---|---|---|
| P1 | Every command waits until **500 ms** after the previous one. | hardware_quirks.md: "use 500 ms for any new node". The old nodes used 250 ms. |
| P2 | Warm handshake: STOP, RESET, 600 ms, `device(2)` **twice** with 500 ms settle, repeat off, shuffle off, EQ 5, volume 30, drain 300 ms, STOP. | Same sequence as all three 2025 nodes. The last STOP defuses the boot auto-play. |
| P3 | **Confirmed start**: STOP, drain, PLAY, then `queryStatus` must say *playing*. Retry 2 re-selects TF twice; retry 3 runs a full handshake. If it's still unconfirmed, `START_FAILED`. | Scarecrow.ino `mp3StartReliable`. |
| P4 | **The module plays until told to stop** (Joe). On FILE_END the driver sends STOP at once. Every track has a **known length**, and at length + 2 s the driver sends STOP itself, even if FILE_END never arrived. | Stops it running on into the next track. |
| P5 | **Verified stop**: STOP, then `queryStatus`. If it's still playing, STOP again (×3). Debounced to 300 ms. | Scarecrow.ino `safeStop`. |
| P6 | **Status poll every 15 s.** Idle but the module says playing: defuse with STOP. A track is running but the module says stopped: end it (FILE_END was missed). Every other poll queries the file index to check it's the right file; this is logged only. | Scarecrow.ino `gDefuseStopPending`. The file index is only logged because the 2025 notes say it can be wrong on some modules. |
| P7 | Unsolicited INIT (module rebooted, power dip): re-handshake, and the track ends `MODULE_RESET` (the INIT caused by our own handshake RESET is swallowed). Error reply 0x40: **only err 5/6/8 (file) end a track as `FILE_ERROR`**; err 1–4 (busy/sleep/serial frame/checksum) are logged + counted (`modErr`) and ignored. SD removed or inserted: logged, and insert re-handshakes. **No bytes from the module for 60 s**: WARN "check 5V/wiring". It re-handshakes **only when no track is playing**; mid-track it warns only (a STOP+RESET would cut the show). **Exception (2026-09-29): a LOOP track (idle bed) is ended as `MODULE_RESET` + re-handshake, so the node restarts it** (`loopRecover` counter); a loop is always "mid-track", so before this a module that died during the ambient waited for the node's 7 min watchdog. | 2026-09-26 bench (below); 2026-09-29 Tree. |
| P8 | A FILE_END within 1.5 s of a PLAY belongs to the previous track and is ignored. | |
| P9 | `play(track, 0, loop=true)`: gapless module-side repeat (CMD 0x08). FILE_END at each wrap is counted (`loopWraps`), not an end; no length stop. | Pumpkin Men idle loop 020. |

Every decision prints a `[mp3]` line. Bench command `mp3` prints the counters:
`plays retry startFail stopResend defuse lenStop modStop stale fileErr modErr reset silent loopRecover idx`.

**Bugs found on the Pumpkin Men bench 2026-09-26 (both nodes rebuilt; Tree not flashed yet):**
- `tick()` took `now` before the P6 poll. The poll's reply moved `_lastRx` past `now`, and `now - _lastRx` wrapped to ~4294967 s, which triggered a false "silent" re-handshake that STOPped every theme ~30–75 s in. Now it uses a fresh `millis()` and a signed compare (`02d1e8e`).
- MD_YX5300 labels every 0x40 reply "file not found". A transient err=3 killed a Corn theme 1:18 in, and audio ran on with no STOP sent (`30c29a6`).
- The module stopped answering polls during theme runs (probably motor/relay noise on the UART), and the P7 check re-handshook mid-track twice. Now it's warn-only while a track plays (`426e8d5`).

## Known track lengths (set in each node's `mp3_player.h`)

| Node | Track | Length |
|---|---|---|
| Spooky Tree | 001 ambient ("JackAttack") | 381 s (measured 6:21.0) |
| Spooky Tree | 002–006 PIR action | 60 s cap (unchanged) |
| Spooky Tree | 007/008/009 theme | 180 s |
| Pumpkin Men | 001/002/003 themes (003 = New mutant, 10-07) | 180 s |
| Pumpkin Men | 004–018 PIR clips | measured each, 4.1–10.9 s (`LEN_CLIP_MS`) |
| Pumpkin Men | 019 idle loop | loop, no length |

The theme files for Tree + Pumpkin Men are rendered to exactly 3:00 with a 500 ms
fade ending on the cut (`~/Documents/show-sync-orchestrator/render_show_tracks.py`,
output `~/Desktop/Spatial audio/master Folder by node (3min)/`). Looping tracks are
never trimmed. A fade can't be done in firmware: at 500 ms pacing the module takes
one command per 500 ms. If a file on the SD card changes length, change it here too. The show-sync-orchestrator checker (`D6`) flags any
show track that isn't 3:00.

## Timing cost

Pacing makes the driver slower than the old one. Boot handshake is about 8 s.
A confirmed start is about 1.5 to 2.5 s (up to about 9 s with retries), and a
verified stop is about 1 s. All of that runs in `loop()`, never in the ESP-NOW
receive path; the nodes defer MP3 work until the inbound queue is drained.

## Never-stuck checklist: every YX5300 node (Tree, Pumpkin Men, Patch L/R)

Written 2026-09-29 after the Spooky Tree was silent "no matter how many power cycles" (Joe: none of these
nodes may ever get stuck). Nothing in firmware survives a power cycle, so a node that stays silent through
power cycles is **power / wiring**. The firmware's job is to recover on its own from everything else:
power dips, module resets, stray commands, and hangs.

**Firmware (every node):**
1. Use this driver (`SafeYX5300`), not a per-node MP3 driver. Idle bed = `play(n, 0, true)` (P9).
2. Idle branch: when the idle loop "finishes" for ANY reason (module reset, loop silence, stop), go back to
   idle and restart it on the next tick; a failed start retries every 30 s, forever. No state may leave the
   node silent with nothing scheduled to restart.
3. Loop watchdog: `esp_task_wdt_init(60, true); enableLoopWDT();` at the end of `setup()`.
   `disableLoopWDT()` when OTA starts; `enableLoopWDT()` when OTA ends AND on the "WiFi connect failed"
   exit. (Longest legit block in `loop()` = a failed start, ~15 s.) Log the arm result.
4. Print `esp_reset_reason()` at boot and flag `BROWNOUT` ("SUPPLY SAG") and `TASK_WDT`/`PANIC`.
5. Module-stall watchdog in the node (Tree pattern): 7 min with no bytes -> restart the idle loop,
   2 more min -> `ESP.restart()`. Signed compares with a fresh `millis()` (the 09-26 wrap bug).
6. Every command path ends back in idle: `CMD_STOP` -> idle (loop restarts next tick); OTA window end or
   failure -> idle; show runs end via `ShowFollower.h` (track end + fail-safe deadlines, host-tested).
   Audited 2026-09-29: no network sender of `CMD_STOP`; `CMD_OTA_MODE` only from the touchpad controller.
7. No persistent state (NVS/RTC) that can keep the node silent across a reboot.
   (The black-box log's files + boot counter are write-only records: they never change behaviour.)
8. **Black-box log (added 2026-10-01, Tree `b6f7b59`; shared since `721f751`):** use `shared/BlackBox`
   (`BlackBox.h` + `NodeDiag.h`),
   route every print through `Log` (incl. `SafeYX5300::setLog(Log)` before `mp3Init()`), `Log.begin()` first
   thing in `setup()`, `Log.tick()` every loop, per-minute `[bb]` status line (state, MP3 last answer, heap,
   loop gap, ESP-NOW rx/junk/drops, last Scarecrow frame), `log*` serial commands, `tools/blackbox_pull.sh tree`
   (make the port an argument). Reason: opening a DevKit's port reboots it, so a stuck state can't be seen
   live; the "it works when I check" on the Tree may have been the check rebooting it (Joe, 10-01).
9. **Serial command input noise-proof (Tree `9b692c9`):** UART0 RX floats with no USB host. Accept only
   printable ASCII <= 64 chars, drop any line containing other bytes, count them (`uartJunk` = noise meter),
   rate-limit "unknown command" logs.
10. **No OTA on show props that must never get stuck** (Tree removed it 10-01, Joe): flash over the bridge.
    Revisit per node.
11. **Self-heal reboot (BUILT 2026-10-01 `721f751`, Tree + Pumpkin Men; Joe: "just don't let it hiccup the
    show in a strange way"):** `shared/Shared_Constants/SelfHeal.h` (host test `props/_tests/test_self_heal.cpp`).
    Triggers: module silent 2 min / idle bed fails to start 3x in a row / idle bed dies 4x in 10 min.
    Reboots ONLY while the node is idle (no theme run, visit, action, OTA, pending op); a trigger mid-show
    waits; only MODULE_SILENT forces after 10 min (prop already dead). Any serial command holds it 10 min,
    `heal off`, max 3 self-heal reboots in a row (RTC counter, cleared after 30 min healthy). Logs
    `[heal] WARN ...` while held, `[heal] HEAL REBOOT: <why> ...` + full status before the restart.
    Replaces item 5's 7-min STALL watchdog. Side effect to know: each reboot sends NODE_ONLINE, the
    Scarecrow answers with a resync burst, and OTHER followers may treat it as a visit (PIR actions held
    up to 10 min, then the session is marked dead). Rare by design.
12. **Slow, ordered cold boot (2026-10-01, Joe: "I don't care if it's slow to boot"):** after power-on /
    brownout wait 20 s, then the MP3 handshake, then the radio (its calibration draws a spike). MAC check
    from the eFuse (`esp_read_mac`) so it needs no radio. Warm resets keep 3 s. Boot ~30 s cold.

**Power / wiring (every node):**
- Amp feed: 16 AWG or heavier for a ~10 ft run (22 AWG at 3-5 A lost ~1 V: the Tree's 09-29 fault).
- Inline fuse at the supply end sized to the WIRE (16 AWG run: 7.5 A, 10 A max), especially on a 20 A supply.
- ~470 uF at the YX5300's 5 V pin; common ground between the amp and the ESP/module supply; short MP3 wires.

**Bench gotchas:**
- USB alone runs the ESP32 + YX5300 (the driver says "PLAY ... confirmed") but the amp is dead: no sound.
  **Check the prop's main power switch first** (Tree 09-29).
- With the prop's main power ON the bridge serial can read garbage (noise over the USB cable). Flash and
  log with the main power OFF.
- **5 V buck on VIN + USB** can back-feed and silence the board (Singing Pumpkins 10-02): see
  `docs/hardware-gotchas.md` (buck + USB entry) before trusting the power-OFF rule on a new prop.
- Boot must show: `Reset reason: ...`, `Loop watchdog armed (60 s, err=0)`, `PLAY track N confirmed (... LOOP ...)`.

**Hardware add-ons (optional, ideas from the Tree 10-01; none built yet):**
- Resistor divider from the supply to a spare ADC pin -> supply voltage in the per-minute log (tests the power theory).
- MOSFET switching the YX5300's 5 V -> the firmware can power-cycle a hung module instead of rebooting the ESP. **Joe 2026-10-26: chosen for NEXT YEAR (2027) on the Tree (and the other YX5300 props); 2026 uses the HA yard-stake power-cycle watchdog instead (CURRENT-BUILD #49).**
- Peak detector on the amp output to an ADC pin -> the firmware can tell the speaker is actually silent.
- PIR on a relay: check the coil has a flyback diode (each click otherwise kicks the shared ground; PIRs trip
  more at night, when the Tree fails).

| Node | Driver loop fix (09-29) | Loop WDT + reset reason | Black box + serial noise-proof + no OTA (8-10) | Status |
|---|---|---|---|---|
| Spooky Tree | yes | yes | **built** `721f751` (+ self-heal, slow boot), not flashed; OTA removed | `9372036` FLASHED 2026-09-29; flash `721f751` next |
| Pumpkin Men | yes | yes | yes (+ self-heal, slow boot, fake-visit fix); OTA kept | **`0eb52cb` FLASHED 2026-10-07** (bridge); bench with main power ON still to do |
| Patch LEFT / RIGHT | yes (SafeYX5300 since 10-01) | yes | yes (+ self-heal, slow boot); no OTA | **rebuilt 2026-10-01**, compiles; never flashed, not bench-tested |

## Echo = module not answering (2026-10-26, Spooky Tree)
Receiving a COMMAND code (0x04-0x39: 0x06 volume, 0x07 EQ, 0x08 loop-play, 0x09 select TF, 0x0B wake, 0x16 stop,
0x19 repeat...) means the ESP is hearing its own TX on RX: TX/RX bridged (wet/corroded connector) or the module
is not driving its TX. Our own queryStatus then comes back as a fake "status 0x0000" (stopped), which the old driver
counted as "module answering". Since `bedf3f6` these are counted (`echo=` in the [mp3] line), logged as
`WARN ECHO`, and never treated as a reply. A start failure prints the raw replies; bench `mp3probe` prints status,
SD file count, folder count, volume, playing file.
