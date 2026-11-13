# Skeleton Narrator A1S — "Burt"

ESP32-A1S (AI-Thinker Audio Kit v2.2). NodeID `0x07` (`NODE_SKELETON_A1S`),
MAC `AA:BB:CC:DD:EE:FF` (bench-confirmed 2026-09-06). Electrically identical to
the Scarecrow board (ESP32-D0WD-V3 rev 3.1, 4 MB flash, 4 MB PSRAM, **ES8388**
codec, DIP 1,2,3-ON = working 1-bit SD).

**Follower**, not the leader — no MQTT, no fleet-cue origination, no leader
heartbeat. The one Tier-2 partner of the Scarecrow (`docs/fleet-communication-tiers.md`
T2-1). Cue path: latch `(theme,tone)` off the fleet broadcast → wait for
`CMD_NARRATOR_2_START` (broadcast burst or the Scarecrow's unicast copy) →
unicast `CMD_ACK` "starting" back (§3.4 H1b handoff, native confirmation, ≤3
tries, non-blocking) → play the line (INTRO/HOLD/OUTRO, selected by the cue's
`hold`/`mainshow` flags) → broadcast `CMD_NARRATOR_2_DONE` ×3.

**OUTRO banter turn (2026-09-24):** a `CMD_NARRATOR_2_START` with the banter bit
(`shared/Shared_Constants/NarratorBanter.h`) means "play exactly this line":
`/burt/banter/<pool>/<idx>_<line>.wav`, no MIDDLE check, then a DONE that echoes
the turn + a played/failed flag so the Scarecrow can match it. Banter cues are
deduped by exact (session, turn), not `seq` ±1. The main-show latch now also
clears on `SHOW_NARRATOR` (a new PRE-SHOW), since the Scarecrow's heartbeat no
longer claims `SHOW_IDLE`.

Has OTA (always-on, hostname `burt-a1s`) as of 2026-09-11 — no USB needed for
updates once deployed, same as Scarecrow.

## Build / flash

```
pio run -e skeleton -t upload      # board on /dev/ttyUSB0 — ⚠ pull the microSD
                                    # card first, see the FLASH GOTCHA in
                                    # platformio.ini; reinsert after
pio device monitor -e skeleton
```

Wireless (once already flashed once over USB): `pio run -e skeleton -t upload
--upload-port <burt-ip>` with `PLATFORMIO_UPLOAD_FLAGS="--auth=$OTA_PASSWORD"`.

## Modules

`pins_a1s.h` · `es8388.{h,cpp}` · `audio_out.{h,cpp}` · `wav.{h,cpp}` ·
`pools.{h,cpp}` (card root `/burt/`) · `middle.{h,cpp}` — **copied from
`../scarecrow-narrator-a1s/src/`** (see TODO). `ota.{h,cpp}` — always-on
ArduinoOTA, mirrors Scarecrow's (no windowing needed, WiFi's already
permanently associated). `burt_link.{h,cpp}` + `main.cpp` are Burt-specific.

Serial: `s`/`S`/`o` speak INTRO/HOLD/OUTRO standalone (no cue needed) ·
`n` next bucket · `l` link log · `w` net · `d` pools · `t` test tone ·
`p` amp · `+`/`-` vol

## Bench status — 2026-09-06 (placeholder audio)

| | result |
|---|---|
| characterization | ✅ ES8388 · PSRAM 4 MB · MAC matches `MAC_SKELETON_A1S` · DIP 1,2,3-ON |
| bring-up | ✅ codec init · I2S clock tree · WiFi ch 11 · ESP-NOW peered w/ Scarecrow |
| **live handshake** | ✅ **3/3 cycles**: cue received, starting-ack SUCCESS 1st try, line played, `NARRATOR_2_DONE` broadcast · `frames=63 cues=3 ack ok=3 fail=0` |

Needs: microSD card, speaker. Log: `bench/live_handshake_burt-side_2026-09-06.md`.

## TODO

- **De-dupe the shared modules.** `es8388/audio_out/wav/pools/middle` are copied
  from the Scarecrow project. Extract to a shared PlatformIO library
  (`props/a1s-narrator-common/`) so the two narrators can't drift.
- Main-show OUTRO cue (`mainshow=1`) — not exercised live yet.
- Burt's own MIDDLE fetch (optional per §2.2 — huge time budget, so low priority).
- Prop I/O (jaw servo) on GPIO 22/23/18/5.
- **Live 2-board verification of the 2026-09-11 HOLD dedupe fix** — needs
  Scarecrow reachable to send a real `preshow`→`hold`×2-3 sequence and confirm
  Burt logs 3 distinct HOLD cues, not 1. Not yet run — Scarecrow was offline
  every time this was attempted so far.
- **GPIO12/microSD flash-strap fault — RESOLVED 2026-09-19, per Joe: it was the
  old microSD card specifically** (one of the two live hypotheses this note
  itself was tracking — see `docs/a1s-board-and-audio-architecture.md`'s
  "Probable cause" write-up, "or this card bridges DATA2 where Scarecrow's
  doesn't"). Not independently re-verified across several power cycles this
  session — the standing advice to confirm a repeatable pattern before fully
  trusting it for a live show still applies to any first real use of the
  replacement card, same as it would for any hardware change.
