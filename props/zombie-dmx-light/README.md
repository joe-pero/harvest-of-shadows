# Zombie Container DMX light (ESP32 -> RS485 -> par can)

> **Read `docs/hardware-gotchas.md` #1-#4 first.** The converter's TXD/RXD labels
> are MCU-side (ESP TX -> module TXD). Getting that wrong cost ~2 h.

This is the higher-resolution companion to `props/zombie-ir-lights/`. An ESP32
listens to the show over ESP-NOW (it never transmits) and drives a 7-channel DMX
par can through a TTL-to-RS485 converter. All three effects were approved by Joe
on the bench on 2026-09-30.

**Status (2026-10-02):** swamp dimmed to 20 % (Joe: "perfect"), live on the light. **(2026-09-30):** show firmware (`env:dmx_show`) FLASHED on the ESP32 and
bench show test PASSED (every transition on time). **Not installed.** The deployed
Zombie Container firmware is untouched. CURRENT-BUILD #32.

## Show

| When | Effect |
|---|---|
| All night (idle) + show 0:00 to 1:13.40 | **SWAMP**: smooth green breathing (5 s, floor 40/255, x3), then an aqua swell (blue to 170), with a slow ripple of about ±10 % |

**Swamp dimmer (2026-10-02):** at full master (255) the swamp was "like daylight" (Joe). The swamp now runs at master `dim` (default **51**/255 = 20 %, NVS `swamp_dim`), tuned live by eye with `dim <n>` over serial. **Joe's pick: 20% = `dim 51`, "perfect"** (set + saved in NVS 2026-10-02, and the code default since `SWAMP_DIM_DEFAULT = 51`; 60/40/10 % tried; at 10 % the green breath dips went black). Heartbeat stays at master 255, zombie flashes at 140 (`kZombieLevel`). Heartbeat (master 255) and zombie flashes (140) stay high on purpose (Joe: "keep them high"). Brightening the light just before the AI snapshot was looked at and **dropped** for now. The light only hears ESP-NOW, the bridge only talks MQTT, and the snapshot is taken the moment someone enters the waiting zone. Joe checked the camera at 20 %: "still looks crisp and clean", so it isn't needed.

| 1:13.40 (WARNING) | **HEARTBEAT**: red lub-dub at 70 bpm, black between beats |
| 1:20.10 (glass break) to 2:49.86 | **ZOMBIE**: Fisher-Yates-shuffled bursts 0.2-4 s, black 0.5-9.5 s; inside a burst, 50 ms white flashes 350-600 ms apart at master 140 |
| 2:49.86 | back to **SWAMP** |

- **Photosensitive limit:** flashes are **≤ 2.9/s** (the WCAG/Ofcom line is ≤ 3/s).
  They were 10/s first, which Joe found hard to look at; the safe rate works better
  ("less upsetting"). The fixture's own strobe channel is **never** used. A "strobe
  lights in use" sign is still good practice.
- The trigger is the same as `zombie-relay-esp01s`: `SET_THEME=ROTLD` or the
  touchpad's `SEGMENT_START=ZOMBIE`, with a `session_id` dedupe.

## Failsafes (nothing reports back)

- **Show clock:** SWAMP at 2:49.86.
- **5 min hard cap.**
- **Start lockout** while a show is running.
- **Abort:** `SEGMENT_END` / `SHOW_STATE` IDLE|OFF go straight to SWAMP.
- **Missed start:** stays on SWAMP.
- **Task watchdog 5 s.** Fixtures usually **hold the last DMX frame** when the signal
  stops. **Boot sends SWAMP within 0.13 s** (measured).
- **Daily idle restart** and a **channel 11 re-lock** every 30 s.
- A `[status]` line every 30 s (fx, show, frames, fleetFrames, rxDropped, heap) that
  can be read through the bridge.

## Wiring (bench-proven)

| From | To |
|---|---|
| ESP32 **TX2 (GPIO17)** | converter **TXD**. The labels are MCU-side (TX→TX); the DIYables tutorial's TX→RXD is **wrong** for this board and cost ~2 h. |
| converter **RXD** | **not connected**. It is the module's output (5 V when VCC is 5 V). |
| converter VCC | **VIN 5 V** (MAX485 ESA is a 5 V part). It worked at 3V3 on the bench; move it to 5 V only with RXD off. |
| converter GND | ESP32 GND |
| XLR pin 1 black / pin 2 red / pin 3 yellow | converter ground terminal / **B−** / **A+** |

Converter: DIYables TTL-RS485 (DIY-MAX485-MODULE), MAX485 ESA + CD4069
auto-direction, no DE/RE pin. A single fixture on a short cable needs no
termination. For a long run that flickers, put 120 Ω across A/B at the fixture.

## Fixture (7-channel flat par, no brand)

Menu `d.xxx` = DMX mode + start address. Press **ENTER** to arm it. Channels
counted from the start address:

| Offset | Channel |
|---|---|
| +0 | master dimmer (colors show nothing at 0) |
| +1 / +2 / +3 / +4 | red / green / blue / white |
| +5 | mode: 0 = manual, 200 = built-in color fade |
| +6 | strobe / program speed. 255 + white = fast strobe. **Unused.** |

The start address is stored in NVS: `base <n>` over serial. The default is
`FIXTURE_BASE=1`, and the bench used `d.001`.

## Serial (115200): bench or yard-bridge-pi

`status` · `start [sec]` (a bench start; it skips the lockout, e.g. `start 68` = 5 s
before red, `start 165` = 5 s before reset) · `end` · `base <n>` ·
`dim <1-255>` (the swamp's master dimmer, live and saved in NVS; `dim` alone prints it) ·
`fx swamp|heart|zombie|off` (a preview with no show clock).

## Builds

- `pio run -e dmx_show -t upload --upload-port /dev/ttyUSB0`: the show firmware.
- `pio run -e dmx_probe ...`: the bench tool (`s <slot> <val>`, `a`, `z`, `inv`,
  `hold`, `swamp`, `heart`, `zstrobe`, and a loopback counter on GPIO4). Used to
  map the fixture and tune the effects.
- ESP32: DevKit `AA:BB:CC:DD:EE:FF` (formerly `props/ir-learner`). Its default
  upload speed hits a stub checksum error, so upload at 115200.
- esp_dmx `DMX_NUM_2` crashes on this core (uart.c `#if` on an enum leaves UART2
  NULL). Use `DMX_NUM_1` only.

## Verified with Joe (2026-09-30)

- Converter on **5 V, RXD off**: swamp OK.
- ESP32 unplugged while running, then replugged: the light **recovers** to swamp.
  (Whether the fixture holds the last frame or blanks during the outage was not
  noted.)
- A fixture power cut keeps `d` mode.
- **Address is final: `d.001` / base 1.**
- Stand-alone light: nothing IR is linked to it (Joe).

## Still to do

- Install. Plug the ESP32 into yard-bridge-pi USB, check that `fleetFrames` climbs
  once it's in range of the yard, watch one real ROTLD show, and post a "strobe
  lights in use" sign.
