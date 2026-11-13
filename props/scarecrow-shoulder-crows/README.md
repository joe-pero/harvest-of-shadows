# Scarecrow shoulder crows (breakout project)

**Status 2026-10-01 (end of day):** board `AA:BB:CC:DD:EE:FF`, firmware FLASHED
`v2026-10-01-shoulder13`, card rewritten (new track, md5 `adbc3f85…`). Head mounted, range 30–140°
(centre 90°; binds past 140), bird-like glance movement, eyes fade with movement, show loops a dense
long-call block (**Joe: "great for the action part"**), idle = short caw bursts with growing gaps +
drowsier movement (bench-verified, see *Idle sound*). **Open decision for tomorrow: idle burst
length** (below). Joe still soldering/mounting. Pick up at *Next session*.

Two crows on the Scarecrow's shoulders: **one turns its head, one tilts** (Joe). They move
slowly when idle and fast during the main show, like the yard crows (`props/crows`), and an
MP3 player loops a crow track that the crows' "caw" gestures are timed to.

## Next session (checklist)
1. **Decide the idle burst length (Joe).** Calls 2→3→4 in the dense block overlap, so a burst can
   only end cleanly after **2 calls (~9 s)** or **5 calls (~23 s)**; the firmware plays a started
   call out, so "3–4 calls" currently always comes out as 5. Options: (a) random mix of 2 and 5
   (firmware only), (b) always 2, (c) rebuild the track so the first 4 calls don't overlap
   (`audio/build_track.py`, then card rewrite + reflash; the show block stays dense after them).
2. After soldering: meter for shorts with everything unplugged (ESP32 5V/VIN to GND, each JZ-MOS
   VIN+/VIN− and OUT+/OUT−, MP3 5V/GND, eyes +/−). Then boot over USB: `[BOOT]`, no `Brownout`.
3. Tilt range: `python3 tools/servo_jog.py 14` (or `SWEEP 14 1 45 135`, widen), result in
   `TILT_MIN/MAX` (still 30–150 placeholders). Joe expects the 2025 tilt servo is fine.
4. Watch + tune movement/eyes with Joe (`MOOD_*`, `EYES_*`, `IDLE_*` in `src/main.cpp`).
5. Tune the player boot delay by ear (`DELAY <ms>`, saved; start 800 ms): caw gesture vs sound.
   **Leave the port alone while listening** (a port open can reboot the board = player restart).
6. Show test: `VISIT` (player OFF, slow moves), `ACTIVE`, `STOP`. Then a real Scarecrow visit.

## Wiring (as built)
| ESP32 | Goes to |
|---|---|
| GPIO 13 | Head-turn crow servo signal. **Blue SG90, 180° positional**, range 30–140°, centre 90° (10-01, mounted) |
| GPIO 14 | Tilt crow servo signal (replacement, moves; jittery in the 10-01 WIGGLE) |
| GPIO 25 | JZ-MOS data. 10 kΩ GPIO 25 to GND keeps the player off during boot |
| GPIO 26 | 2nd JZ-MOS data -> LED eyes, PWM 1 kHz. 10 kΩ GPIO 26 to GND. Fade on with movement, off when still (`EYES AUTO`, default) |
| GND | Both JZ-MOS signal GNDs + every supply − (shared ground) |

| 5 V supply | Goes to |
|---|---|
| + | Both servos' red, JZ-MOS VIN+ |
| − | Both servos' brown/black, JZ-MOS VIN−, ESP32 GND |
| JZ-MOS OUT+ / OUT− | MP3 player 5V / GND (not also its micro-USB) |

- JZ-MOS = dual AOD4184 switch module, 3.3 V trigger OK, switches in µs; it switches the low
  side (OUT−).
- Pins to avoid on the ESP32: 0/2/5/12/15 (boot strapping), 6–11 (flash), 1/3 (USB serial),
  34–39 (input only).
- Optional: 470–1000 µF across the MP3 module's 5 V (softens the switch-on surge).

## Behaviour (`src/main.cpp`)
**Movement = bird-like glances (shoulder7, 2026-10-01; Joe: "make the loop look real").** Not a
smooth continuous wander: quick snaps (ease-out, ~70 ms + 2.2 ms/deg, about SG90 speed) then a dead
hold. The head picks small looks (10–35° off forward), big looks (outer end of a side), double-takes
(look, 150–350 ms, snap most of the way back), twitches (3–8°) and rests (forward, 5–20 s). The tilt
cocks 15–40°, levels, twitches, rests, and often cocks a beat (100–300 ms) after a head look. Three
moods set weights/holds/speed: IDLE, NARRATION (calmer), SHOW (busier, faster). The old
"still 30–150 s, then 20–60 s of wandering" event model is gone.

**Idle sound (shoulder12/13, Joe 10-01 "three or four caws, then slower to even slower"):**
power on → the first `IDLE_BURST_CALLS_MIN..MAX` (3–4) calls → off at the end of the last call
(**any call already started is played out**, shoulder13: shoulder12 cut the overlapping 4th call
0.7 s in) → wait `idleGapMs(step)`. Bench 10-01 gaps: 34, 43, 68, 86, 167, 255 s (the ±20 %
jitter is applied after the 240 s cap, so up to ~288 s). Each burst also makes the IDLE mood
drowsier (longer holds/rests, more rests, fewer big/double looks, softer snaps). After a show
(`gQuick` true → false) the step resets to 0 and a burst plays right away. `PLAY` = burst now.
`STATUS` prints `idle step=… nextBurst=… s`.

**Eyes (shoulder8):** fade up in 200 ms when either crow moves, stay 400 ms after it stops, fade out
over 1.5 s (gamma ~2). Own PWM timer via ESP32Servo's `ESP32PWM` (no clash with the servos).

| Show phase | Player | Movement |
|---|---|---|
| Idle (no visit) | **Short bursts** from the top of the track, then OFF; gaps grow 30 s ×1.5 → 4 min (±20 %), reset by a show | IDLE glances, **drowsier each burst** (5 bursts = fully drowsy) |
| Narration (`SHOW_NARRATOR`) | **OFF** (voices first, CURRENT-BUILD #27) | NARRATION glances (calm) |
| Main show (`SHOW_ACTIVE`) | ON; **loops only the 0–38.6 s long-call block** (restart at `CROW_SHOW_BLOCK_MS`) | SHOW mood glances |
| Every call in the track | — | Both crows do a quick caw gesture (head swing, tilt cock) |

- **One fixed sound track for every phase** (Joe: "a crow sound is a crow sound"). Movement is
  what changes.
- Sync: the player is powered through the JZ-MOS, so power-on = the track from the top. Caw
  gestures fire at `onset (crow_track.h) + boot delay`. **The player is power-cycled at the end of
  every 5:00 loop** (1.5 s off), so the timing can't drift.
- Show cues: `shared/Shared_Constants/ShowFollower.h`, the same rules as `props/crows`, Tree and
  Pumpkin Men (docs/show-follower.md). Only the Scarecrow's MAC + sender + valid CRC count. Fast
  mode ends at SHOW_OFF (after 3:00) / STOP / the 230 s fail-safe / a 15 min hard backstop. A
  narration that never reaches ACTIVE times out in 5 min.
- **Brownout guard:** after a brownout reset the player stays OFF until `UNMUTE`, and a WARN is
  printed, so a power fault can't become a reset loop.
- Board: ESP32-D0WD-V3, MAC `AA:BB:CC:DD:EE:FF` (since 2026-10-01; the first board `AA:BB:CC:DD:EE:FF` went to another project). Factory AT-firmware backup in `_board_backups/` (git-ignored). Build pinned to `espressif32@6.12.0`, like
  `props/crows`.

**Bench serial (115200):** `STATUS`, `VISIT ACTIVE OFF IDLE STOP FAKE` (show cues as if from the
Scarecrow), `PLAY` (restart the player), `MUTE`/`UNMUTE`, `DELAY <ms>` (boot delay, saved),
`WIGGLE <pin> [n]` (60°↔120°), `SWEEP <pin> [n] [lo] [hi]`, `SWAP` (head<->tilt pins, saved),
`EYES AUTO`/`EYES ON`/`EYES OFF`, `PLAY` (idle: burst now), `POS <pin> <deg>` (park one servo, idle paused 10 min), `SERVOS OFF`/`SERVOS ON` (detach/re-attach; NOT saved, a reboot re-attaches
and turns the player back on), `HELP`. WIGGLE/SWEEP pause the idle moves for 60 s. **Opening the
serial port can reset the board** (and restart the player).

## Audio track
- Source: the 2025 card's `Crow sounds.mp3` (2 h 54 min, 49 kbps) = ~15 distinct calls repeated
  ~300 times between stretches of digital silence. A copy is in `audio/source/` (git-ignored,
  65 MB; back it up to the backup drive). Source md5 `0714e6b80610f075e937b8eeffe9e9bd`.
- `python3 audio/build_track.py <source.mp3>` cuts each call once (`audio/calls/`) and builds
  `audio/crows_loop.mp3`: **5:00, 14 calls: a 0.6 s caw, then a dense 38.6 s block of the long
  calls (2.5–5 s, short gaps/overlaps, 2026-10-01 for the show), then 5 sparse calls 30–60 s apart**
  (fixed seed, the same track every build). It also writes `src/crow_track.h` + `audio/track_events.json`
  (audible onsets, verified within ±6 ms by re-analysing the MP3). It scans at 8 kHz mono: the
  full file at 44.1 kHz stereo needs ~4 GB and got OOM-killed once.
- If the track is rebuilt, **rewrite the card and reflash** (the header must match the card).

## Player card
120 MB card, **FAT16 label `CROWS`, only `crows_loop.mp3`** (md5
`adbc3f855294c63543b9438e569e791e` (2026-10-01 track; was `db4682a9…`), verified after an unmount/remount). It arrived with a corrupted
FAT (pulled without an eject; the kernel set it read-only), so it was reformatted with a guarded
script (refuses unless the device is removable + USB + ~126 MB; the Workshop's `sdc1` 128 MB is
a partition of the 5 TB media drive, NOT a card). **Eject cards before pulling them.** The 2025
15 GB card is untouched.

## Power incident (bench 2026-09-29)
- Flashed, clean boot, the player switched on and a caw was heard (JZ-MOS + card + player OK).
  The head crow moved on GPIO 13; the tilt crow on GPIO 14 never moved.
- Then a brownout reset loop (`Brownout detector was triggered`, reset reason 9, USB errors
  -71): the servos + amp were all fed through the ESP32's USB/VIN, and each boot switched the
  player on again.
- **The VIN wire melted slightly. Root cause (Joe): a failing servo** drawing heavy current,
  which is also why the tilt crow never moved.
- Rules: servos + player on their own fused 5 V supply, never through the ESP32's VIN;
  proper-gauge wire; meter for shorts before re-powering after any heat damage.

## Bench 2026-10-01
- First board went to another project; new board flashed (old factory AT firmware backed up to
  `_board_backups/`, git-ignored). First upload "chip stopped responding", retry OK.
- Head: the MG90S didn't move on GPIO 13 OR on the GPIO 14 wire (= servo/plug, not the pin).
  Its replacement spun round and round on SWEEP: a **continuous-rotation (360°) servo**, which
  takes the signal as speed, not position — unusable for the head. **Use a 180° servo; check by
  hand unpowered: it must hit a stop at each end.** The blue SG90 is 180°: swept 45–135 → 35–145
  → 25–155 with Joe, then wider on one side only (POS/SWEEP to 175): **25–175 kept**. Centre is
  now a fixed `HEAD_HOME` 90°, not the range midpoint (which would be 100° = head turned).
- Eyes: 2nd JZ-MOS on GPIO 26, lit on `EYES ON`.
- "Crow audio sounds like the ESP32 backfeeds the player" = the player being power-cycled by every
  flash/port-open reboot (it turns on at boot), not a backfeed. Listen with the port left alone.
- Garbled serial once during a servo move = servo noise on the shared supply/ground (recovered).

- Mounted head buzzed (binding) on one side of 25–175 → `tools/servo_jog.py` (arrow-key jog via
  `POS`, head only, marks LOW/HIGH): Joe found 25 / 140, then low end bumped to 30 → **30–140**.
- Show sound: before, show mode replayed only the first 1.7 s caw every ~22 s. Track rebuilt with a
  dense long-call block (0–38.6 s) the show loops (Joe: great). Idle reworked to bursts (above).
- Card writing: the `CROWS` card is `/dev/sdg1` in the Workshop's multi-slot USB reader. **Don't
  `udisksctl power-off` it**: that powers off the whole reader (all slots vanish until it is
  replugged). Unmount only, then pull.
- Serial on this host: this board also gets the line-tail repeat flood (hardware-gotchas
  §6C:C8:40); read with a real-time line filter, not one bulk read.

## 2025 hardware (replaced)
Arduino Nano, servos on D2 + D3, a relay switching 5 V to an AITRIP "TF Card U Disk MP3 Decoder
Player Module with Audio Amplifier" that looped the 3 h file. The Nano sketch is not on the
Workshop (the servo ranges are found again with SWEEP).

## Later
- Pull the motion engine into a shared lib with `props/crows` (forked on purpose for now, so the
  flashed and verified yard crows weren't touched before Halloween).
- Mounting on the Scarecrow: weight, cable routing, servo noise near its speaker.
