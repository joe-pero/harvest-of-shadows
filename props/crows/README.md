# Crows (yard crow pair): 2025 "Crow animator", now following the 2026 show

Two crows, each with a head turn and a tilt (4 servos), on one ESP32. They move
on their own with slow random moves, and go fast while the Scarecrow runs a
show. Moved into this repo 2026-09-29 from `~/Documents/Halloween-Dev/PlatformIO/Projects/Crows`
(no git history before then; the pre-2026 source is in `_board_backups/*.pre-ch11`).

## Board
- ESP32-D0WD-V3 rev3.1, 4 MB flash, CP210x USB-serial, MAC `AA:BB:CC:DD:EE:FF`
  (not in `ProjectConstants.h`: the crows only listen, they never send).
- Firmware banner: `Crow animator starting v2026-09-29-show2`.
- Build pinned to `espressif32@6.12.0` (Arduino 2.0.17 / IDF 4.4.7). 7.x breaks the
  ESP-NOW receive callback.
- Uploads often fail once with "chip stopped responding" while writing the app
  (0x10000). An immediate retry has worked every time.
- **Servos need their own 5 V supply (≥3 A, common ground) in the yard.** On the bench
  they ran off USB, which is fine for light moves but can brown out the board.

## Pin map (bench-verified with Joe, 2026-09-29)
| Servo | GPIO | Range |
|---|---|---|
| Crow A head turn | 13 | 5°–122° |
| Crow B head turn | 14 | 5°–122° |
| Crow A tilt | **12** | 5°–**130°** (`TILT1_MAX`) |
| Crow B tilt | 27 | 5°–122° (`TILT2_MAX`) |

The 2025 animator code had tilt A on GPIO 26 and never moved it. The prop is wired
like `src_range_explorer` (13/12/14/27). GPIO 12 is a strapping pin: nothing may hold
it HIGH at boot (fine with the servo, boot verified).

## Behaviour
- **Ambient:** an event every 30–180 s, lasting 30–120 s, moves 0.6–1.6 s and holds
  0.8–3 s. Tilts favour the extremes.
- **Quick (show):** moves 0.3–0.9 s, holds 0.2–0.9 s, events back to back.
- **Show follow** (`shared/Shared_Constants/ShowFollower.h`, the same rules as Tree and
  Pumpkin Men, `docs/show-follower.md`):
  - quick from `SHOW_NARRATOR` (theme picked) through `SHOW_ACTIVE`
  - back to ambient on `SHOW_OFF`/`SHOW_IDLE` after ≥3:00 of show, OFF before ACTIVE
    (visit reset), or `CMD_STOP`
  - fail-safes: 230 s after ACTIVE, 5 min in a visit without ACTIVE, a 15 min hard
    backstop on quick
- Frames count only with valid magic/version/CRC, from `MAC_SCARECROW_A1S` with
  sender = `NODE_SCARECROW_A1S`. They are queued from the WiFi callback (16 slots,
  drops logged) and handled in `loop()`.
- ESP-NOW listens on ch 11 (`ESPNOW_CHANNEL`, NETWORK_CONSTRAINTS C1).
- The 2025 text trigger `ZONE_FAR` still works (nothing sends it in 2026).

## Bench serial (115200)
`VISIT`, `ACTIVE`, `OFF`, `IDLE`, `STOP`, `FAKE` (wrong sender, must be ignored),
`SHOW` (status + counters), `QUICK [s]`, `AMBIENT`, `WIGGLE <pin> [n]`,
`SWEEP <pin> [n] [lo] [hi]`. The show cues go through the same handler as radio
frames. WIGGLE/SWEEP pause the animation for 60 s.

Opening the serial port can reset the board. Right after opening, the port can also
replay one stale line hundreds of times: flush the input after the reset, then read.

## Verified 2026-09-29 (bench, servos connected)
- Clean boot, channel 11, pins 13/14/12/27.
- FAKE ignored; VISIT goes quick; ACTIVE plays; early OFF/IDLE ignored; STOP and OFF
  before ACTIVE go back to ambient; no OFF = FAIL-SAFE at 231 s.
- SWEEP of all 4 servos with Joe: tilt A good up to 130°; tilt B and the heads good at
  5–122°. Fast mode: tilt A reached 129°, tilt B stayed ≤122°.
- `props/_tests/run.sh`: show_follower ALL PASS.
- Fixed in review: `quickProfileActive()` wasn't safe across the ~49.7-day `millis()`
  wrap (an expired window read as active = stuck quick). It now uses a signed difference.

**Not yet tested:** a real Scarecrow visit over the air; the 5 min and 15 min fail-safes
on hardware (they're in code, and the 5 min one is the shared follower's host-tested rule).

## History
- The unused 2025 `Msg` was removed from `src/protocol.h` (it clashed with ProjectConstants).
- Serial garbage at every baud on the first day was a bad USB cable, not a boot loop.
- A full flash backup stalled at 0x40000 twice on that cable. Only `_board_backups/`
  source copies were kept.
- `src_range_explorer/` = the 2025 servo range tool (same pins as this prop).
  `include/protocol.h` is an unused 2025 leftover.
