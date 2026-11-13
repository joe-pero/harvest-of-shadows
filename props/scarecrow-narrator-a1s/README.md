# Scarecrow Narrator A1S

ESP32-A1S (AI-Thinker Audio Kit v2.2) narrator prop **and** the show's fleet
leader / WiFi→ESP-NOW bridge. NodeID `0x06` (`NODE_SCARECROW_A1S`),
MAC `AA:BB:CC:DD:EE:FF`.

Design: `docs/a1s-board-and-audio-architecture.md`,
`docs/fleet-communication-tiers.md`, `docs/_history/dialogue-architecture.md`.

## Build / flash

```
pio run -e scarecrow -t upload        # board on /dev/ttyUSB0
pio device monitor -e scarecrow
```

`platform espressif32@7.0.1` (arduino-esp32 3.0.7). `-DBOARD_HAS_PSRAM`.
One LDF workaround in `platformio.ini` (explicit `-I` for the bundled `FS`
library — platform 7.0.1 quirk).

## Modules

| file | role |
|---|---|
| `pins_a1s.h` | pin map + node identity (bench-confirmed 2026-09-06) |
| `es8388.{h,cpp}` | ES8388 codec — DAC-only init, LINE OUT, volume |
| `audio_out.{h,cpp}` | I2S master (MCLK on GPIO0), PCM16 stereo, mono→stereo, tone |
| `wav.{h,cpp}` | canonical RIFF/PCM16 reader → `audio_out` (any rate) |
| `pools.{h,cpp}` | SD `/scarecrow/<seg>/<theme>_<tone>/NNN.wav` + `index.csv` + §2.3 fallback ladder |
| `middle.{h,cpp}` | live MIDDLE — HTTP push-then-play into PSRAM (§2.2) |
| `fleet.{h,cpp}` | WiFi STA ch 11 + ESP-NOW: fleet burst (H1), Burt handshake (H1b, HOLD-flagged), NARRATOR_2_DONE + NODE_ONLINE detection, `CMD_HEARTBEAT` every 10 s (liveness only, no show state, 2026-09-24), Burt presence probe + turn-matched banter cue/wait, coexistence self-test |
| `aside.{h,cpp}` | Tree asides (2026-09-24): on the Spooky Tree's `CMD_PROP_MOTION`, if the Scarecrow is idle (no visit, no SHOW_ACTIVE window, 30 s quiet after any cue), say one short line from `/scarecrow/aside/tree/NNN.wav`; 1.5 s after the motion, 90 s cooldown, last 8 not repeated. Stops the instant MQTT data (a show's params/cue) is waiting, checked on the playing task. No extra radio traffic |
| `banter.{h,cpp}` | OUTRO banter conductor (2026-09-24): plays Scarecrow's lines, cues Burt per turn, waits for his DONE for that exact turn, stops on any failure; reads `/scarecrow/banter/index.csv`. Protocol: `shared/Shared_Constants/NarratorBanter.h`. Design: `docs/DESIGN-outro-banter-playback.md` |
| `ota.{h,cpp}` | always-on ArduinoOTA — WiFi is already permanently associated, no windowing needed (unlike the touchpad's on-demand OTA service) |
| `mqtt_client.{h,cpp}` | real cue transport — PubSubClient + ArduinoJson, `harvest/show/params`(retained)/`cue` subscribe, `harvest/show/leader/status` LWT + publish |
| `main.cpp` | boot/serial diagnostics **and** the real MQTT-driven phase state machine (preshow/hold/mainshow/**encore**/reset). MAIN-SHOW = the bridge-chosen banter exchange (matched OUTRO pool only if no banter line was heard); a `reset` during the open SHOW_ACTIVE window is deferred to the window's end (2026-09-24) |

## Cue transport (2026-09-11) — the real front-yard trigger

`main.cpp` is driven by MQTT now, not serial. `harvest/show/params` (retained:
`session_id`/`theme`/`tone`/`cycle`, theme/tone as the same lowercase strings
`pools.cpp` uses for bucket names — `"corn"|"rotld"|"pumpkins"`,
`"friendly"|"scary"`) latches the show; `harvest/show/cue`
(`{"session_id":..,"phase":"preshow"|"hold"|"mainshow"|"encore"|"reset"}`) drives it.
Since 2026-09-24 params may also carry `"banter": {"pool","idx","lines"}` (the
OUTRO exchange the bridge picked) and `"encore": true`. `harvest/show/leader/status`
reports the last banter run (`banter`, `banter_outcome`, `banter_heard`, `banter_total`)
and the tree asides (`tree_motion_rx`, `aside_played`, `aside_aborted`, `aside_busy`).
Broker: `192.168.1.x:1883`, user `halloween` (same broker as
`Garage_Presence_System`'s relay_controller — a separate repo). No real
publisher exists yet (`orchestration/showsim/controller.py` only logs what it
would publish) — bench-test with `mosquitto_pub`, see `BUILD_LOG.md`'s
2026-09-11 entry for the exact commands.

OTA: `scarecrow-a1s` hostname, `OTA_PASSWORD` from `wifi_credentials.h`,
always listening (no window to open first). `pio run -e scarecrow -t upload
--upload-port <ip>` with `PLATFORMIO_UPLOAD_FLAGS="--auth=$OTA_PASSWORD"`.

## Serial commands (main.cpp) — bench diagnostics, independent of the MQTT path

`t`/`T` narrator self-test (no radio/Burt/MIDDLE) · `n` next bucket ·
`c` run a cycle · `C` fixed-session cycle (deterministic MIDDLE URL) ·
`a` toggle auto-cycle · `x`/`X` coexistence test (without/with playback) ·
`b` cue Burt now · `A` one tree aside now (still requires idle) · `e<pool> <idx>` run a banter exchange (pool 0 scary, 1 friendly, 2 encore_scary, 3 encore_friendly; e.g. `e0 12`) ·
`h` handshake rollup · `w` net + MQTT status · `d` pool table ·
`r` dump ES8388 regs · `p` toggle on-board amp · `+`/`-` volume

## Bench status — 2026-09-06 (placeholder audio)

| step | result |
|---|---|
| 1 characterization | ✅ ES8388 · PSRAM 4 MB · DIP 1,2,3-ON = 1-bit SD OK · GPIO map |
| 2 bring-up | ✅ codec init clean · I2S clock tree up · tone streams (not heard — no speaker) |
| 3 playback path | ✅ WAV parse + stream + rate-switch · pools/ladder compiled (placeholder path, no card) |
| 4 WiFi + ESP-NOW | ✅ ch 11 · **coexistence PASS** (60 broadcasts + 200 KB GET + playback) |
| 5 integration | ✅ full cycle · MIDDLE success + deadline paths · H1b proceeds-anyway |

**Needs real hardware:** microSD card (SD mount), powered speaker (audible check),
Burt's board (handshake SUCCESS + `CMD_ACK` receive). Logs in `bench/`.

## TODO before production

- Burt-side (`skeleton-narrator-a1s`) HOLD-flag parsing + Burt's own OTA/MQTT —
  today only Scarecrow understands the HOLD bit; Burt safely ignores it (tone
  decode unaffected) but doesn't yet distinguish a HOLD retrigger.
- The real Python/orchestration MQTT publisher (Frigate → `theme_deck.py` →
  broker) — `orchestration/showsim/controller.py` still only logs what it
    would publish.
- Dynamic MIDDLE deadline (`main.cpp`'s `middle_fetch_and_play(..., 4000, ...)`
  is still the bench placeholder — should model "INTRO_len - 1s").
- Prop I/O (jaw servo / LED eyes) on GPIO 22/23/18/5.
- Raise `SDMMC_FREQ_DEFAULT` → highspeed once a real card proves stable.
- MQTT keepalive vs. long blocking calls: `mqtt_tick()` isn't serviced during
  the ~20-40s blocking INTRO/MIDDLE/OUTRO playback, so the broker can drop the
  connection on keepalive timeout mid-phase (observed 2026-09-11 bench test —
  reconnects cleanly afterward, retained `params` redelivers, but a cue
  published during that exact window could be missed since QoS 0 doesn't
  queue for a disconnected client). Timing margin in the real show design
  (cues are tens of seconds apart) makes this low-risk, not zero.
