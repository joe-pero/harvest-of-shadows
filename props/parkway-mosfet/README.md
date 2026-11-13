# Parkway MOSFET lights (ESP32 -> 4 single MOSFET modules)

Parkway lights (the old ESP-01S/WS2811 parkway nodes are retired). An ESP32 listens to the
show over ESP-NOW (never transmits) and drives 4 single-channel 12 V MOSFET modules.
Same timeline, trigger and failsafes as `props/zombie-dmx-light` (Joe: that's the
current, correct format). CURRENT-BUILD #40.

**Status (2026-10-07):** fw `07b3a42` FLASHED (`848ef51` = + 10 min expiry on bench `fx` previews, built, flash at the next USB session; not needed for the show). **Full show APPROVED by Joe on the AC/DC
supply**: swamp ("looks good"), then red alert + amber strobe via a one-shot `nextboot`
run ("This is perfect"). Not installed; not yet seen with a real ESP-NOW trigger.

## Power rule (this project blew a 4-ch MOSFET board + an ESP32)

- **Never PC USB and the AC/DC converter on VIN at the same time.**
- USB bench/flash: converter's 5 V OFF VIN, ESP32 on USB only.
- Running: USB unplugged, converter 5 V -> VIN, GND -> GND (shared with the module
  signal GNDs and the 12 V negative).
- Judge colors/darkness only on the converter: on PC USB every channel glowed faintly
  at 0 (USB ground); gone on the converter (`docs/hardware-gotchas.md`).
- No OTA in this firmware: every update is a USB session.

## Channels (probed 2026-10-07, Joe's eye)

| GPIO | Color | Used for |
|---|---|---|
| 25 | red | RED ALERT |
| 26 | green | SWAMP |
| 27 | aqua blue | SWAMP aqua swell (on/off only from `b225a49`) |
| 33 | amber | the "white" STROBE (Joe) |

No strapping pins (avoid 0/2/5/12/15, 1/3, 6-11, 34-39). PWM 1 kHz, 12-bit, gamma 2.2.

## Show

| When | Effect |
|---|---|
| All night + show 0:00-1:13.40 | **SWAMP**: 3 green breaths (5 s) then an aqua swell, slow ripple. `dim` (default 128, NVS) |
| 1:13.40 | **RED ALERT**: red lub-dub, 70 bpm |
| 1:20.10-2:49.86 | **STROBE** on amber: shuffled bursts, 50 ms flashes, <= 2.9/s (photosensitive limit) |
| 2:49.86 | back to SWAMP |

Trigger: `SET_THEME=ROTLD` or touchpad `SEGMENT_START=ZOMBIE`, session dedupe. Failsafes:
local clock, 5 min cap, start lockout, END / SHOW_STATE idle|off -> swamp, watchdog 5 s,
outputs LOW first thing at boot, daily idle restart, ch 11 re-lock every 30 s.

## Serial (115200)

`status` · `start [sec]` (`start 68` = 5 s before red, `start 165` = 5 s before reset) ·
`end` · `dim [1-255]` · `fx swamp|alert|strobe|off` ·
`nextboot [sec]` (runs the show ONCE at the next boot from <sec>, default 63 = 10 s before red;
erased before use; for checking on the converter, since USB adds a faint glow). Opening the port resets the board.

## Builds

- `pio run -e show -t upload --upload-port /dev/ttyUSB0`: show firmware.
- `pio run -e probe ...`: bench tool (`1`-`4` solo, `0` off, `L<ch> <v>`, `D<ch>` ramp).
- ESP32-D0WD-V3 `AA:BB:CC:DD:EE:FF` (CP2102), upload at 115200.
