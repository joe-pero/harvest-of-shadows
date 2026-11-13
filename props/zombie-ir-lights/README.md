# Zombie Container IR lights (ESP-01S)

Background lighting for the Zombie Container. An ESP-01S listens to the show over
ESP-NOW (it never transmits) and drives the container's cheap 44-key-remote LED
controller over IR, so those lights roughly follow the PicoDMX show. It is a
low-resolution background effect by design (Joe, 2026-09-30). A higher-resolution
DMX light is planned separately.

**Status (2026-09-30):** firmware `6797b52` FLASHED on two units, both boot to
SWAMP (unit 1 ran 11 min, unit 2 30+ min, heap flat at 51 KB, no restarts). **Bench
show run with the ESP32 bench sender not done yet. Not installed.** CURRENT-BUILD #31.

The deployed Zombie Container firmware (relay + touchpad) is **not** touched by this
project.

## What it does

| When | Lights | Container state |
|---|---|---|
| All night (idle) | **SWAMP**: green breathing x3, then one quick aqua breath | night default |
| show 0:00 to 1:13.40 | SWAMP | CONTAINED |
| 1:13.40 | **RED ALERT**: red fade up/down (~3.6 s), dim floor | WARNING |
| 1:20.10 | **ZOMBIE**: DIY1 black 6-15 s, then a 1.5-4 s white strobe burst, repeat | glass break / loose |
| 2:49.86 | back to SWAMP | reset |

Timeline = `docs/zombie-container-dmx-timeline.md` (Joe's measured show), same
numbers as `props/touchpad-ota-controller/src/dmx_timeline.h`.

## Trigger

Same as `props/zombie-relay-esp01s`, the board that actually fires the PicoDMX:
the Scarecrow's `CMD_SET_THEME = THEME_ROTLD`, or the touchpad's
`CMD_SEGMENT_START = SEGMENT_ZOMBIE`. There is one dedupe per `session_id` (each
arrives 3x). The touchpad sends **no** phase messages and no END when the show
finishes normally, so the node runs its own show clock from the start.

## Failsafes (Joe: nothing reports back, it has to fall back on its own)

- **Show clock:** returns to SWAMP at 2:49.86 on its own.
- **5-minute hard cap:** no show state outlives 5 min, whatever arrives or doesn't.
  It is separate from the clock logic.
- **Start lockout:** new starts are ignored while a show runs, so repeats can't keep
  it in RED/ZOMBIE.
- **Abort:** `CMD_SEGMENT_END` (touchpad e-stop/reset) or `CMD_SHOW_STATE` IDLE/OFF
  (Scarecrow reset) sends it back to SWAMP right away.
- **Missed start:** it just stays on SWAMP (fine, per Joe).
- **Idempotent IR only:** colors, DIY1 black, brightness clamped at min/max. The
  fades re-send the color and re-assert the dim floor every breath. A lost or
  doubled IR frame heals within one cycle. **POWER (a toggle) is never sent.**
- **Boot = SWAMP.** There is a daily preventive restart while idle (no OTA, not
  meant to be pulled). The ESP8266 watchdogs reset a hung loop, which also lands on
  SWAMP.
- **Channel guard:** it re-locks to ch 11 every 30 s if it drifts.

## Hardware

- **IR TX module (Dorhea 38 kHz):** S to **GPIO3** (the RX pin, not a boot strap
  pin). VCC on the **dedicated 5 V module**, ESP-01S on its **own 3.3 V module**,
  **grounds shared** (Joe). Before wiring, check that S reads about 0 V with the
  module on 5 V and S unconnected.
- **Beam:** ~20 degrees, ~1.3 m (4.3 ft) at 5 V. Aim straight at the controller's
  IR eye. Off-angle looks exactly like a dead module (bench 09-30: modules "failed"
  until re-aimed). The unit goes in a clear NEMA box, and Joe positions it by eye.
  The commands are frequent enough that it's obvious when it's working.
- **Serial is TX-only** (GPIO3 belongs to the LED): logs only, no serial commands.
- **Controller facts (Joe verified 09-30):** a power cycle restores the last state.
  **DIY1 is mixed to black** (0,0,0) and survives a power cycle; it is the node's
  "off". Keep it black.

| Unit | MAC | Notes |
|---|---|---|
| 1 | `AA:BB:CC:DD:EE:FF` | flashed `6797b52`, boot-verified |
| 2 (spare) | `AA:BB:CC:DD:EE:FF` | flashed `6797b52`, boot-verified |

## Flashing (USB ESP-01S adapter, CH340)

1. Adapter switch to **PROG** (or hold GPIO0 to GND), then plug it in.
2. **One** connection only, no reset. A second connect on this adapter fails with
   "Timed out waiting for packet header" or "serial data stream stopped". Replug in
   PROG to retry.
3. Check the MAC is **not** the relay (`AA:BB:CC:DD:EE:FF`) before writing.

```
pio run -e zombie_ir
esptool --port /dev/ttyUSB0 --baud 115200 --before no_reset --after no_reset \
    write_flash 0x0 .pio/build/zombie_ir/firmware.bin
```

4. Switch to **UART**, replug. Expected log at 115200:
   `[zombie-ir] boot ... MAC ...`, `[espnow] listening on channel 11`,
   `[fx] NONE -> SWAMP (boot)`, then a `[status]` line every 30 s.

## Bench testing without touching the live show

`env:bench_sender` (ESP32 DevKit) sends only a 12-byte bench frame
(`include/bench_frame.h`) that every fleet receiver drops. That was audited
2026-09-30: all harvest-of-shadows receivers + Halloween-Dev eave/zombie/parkway/
stain-glass/area-lights. It **cannot** send a real Msg, so it can't fire the
PicoDMX.

| Command | Does |
|---|---|
| `start [sec]` | start the show clock, optionally jumped ahead (`start 68` = 5 s before red, `start 165` = 5 s before reset) |
| `end` | like the touchpad's SEGMENT_END |
| `off` | like the Scarecrow's SHOW_STATE OFF |
| `soak <min>` | a full show every `<min>` minutes (overnight soak), `soak 0` stops |

Bench starts skip the start lockout on purpose, so you can jump around. The lockout
only gets exercised by a real show.

**Still to run:** `start 68`, `start 165`, `start` + `end`, an overnight `soak 10`,
then one real ROTLD show after install.

## Effects: how they were tuned (bench, 2026-09-30, `props/ir-learner/`)

- Codes: `props/ir-learner/codes.md`. All are NEC, the standard 44-key table,
  replay-verified.
- IR frames take ~108 ms each, so fades are brightness **steps**. Brightness
  commands use a 20 ms gap instead of the NEC ~40 ms (bench-proven). That is as fast
  as it goes.
- **White strobe:** WHITE, BRIGHT UP x10, FLASH, QUICK x10 **with its original
  spacing** (~2.6 s). With tighter spacing (30 ms / short-gap QUICKs) the controller
  dropped FLASH, which showed as solid white. An ESP-made WHITE/black toggle strobe
  looked like a slow random flash and was rejected. The controller's FLASH mode is
  what works.
- Background only: the strobe visibly starts ~1-2 s after 1:20.10 (Joe: fine).
