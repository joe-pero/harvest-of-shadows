# Hardware gotchas (known issues: check here BEFORE debugging)

Hard-won bench findings that cost real time. Each entry has the symptom, the
actual cause, the fix, how to spot it fast, and where it applies. Read the
relevant section before wiring a similar part. Add an entry whenever something
takes more than ~15 min to root-cause.

---

## 1. DIYables TTL-RS485 module: TXD/RXD labels are MCU-SIDE (ESP TX -> module TXD)

**Cost: ~2 hours on 2026-09-30** (`props/zombie-dmx-light/`).

**Part:** DIYables "TTL to RS485 Converter Module, Hardware Auto Flow Control
3.3V 5V" (Amazon B0H28NVFSC, model DIY-MAX485-MODULE, 5-pack). Blue board with a
4-pin header (GND, RXD, TXD, VCC), terminals (ground in Chinese, B-, A+), and a
MAX485 ESA + CD4069UBM (auto-direction, no DE/RE pin).

**Symptom:** the DMX fixture never responded. That held with all 512 slots at 255,
both A/B polarities (swapped wires AND software TX inversion), on 3.3 V and 5 V,
and with two different modules. The module's RXD LED lit and even followed the
signal, so the wiring looked right.

**Cause:** the pin labels are named **from the microcontroller's side**:
- **ESP32 TX -> module TXD** (TXD is the module's driver INPUT)
- module **RXD** = the module's receiver OUTPUT (to the MCU's RX)

The vendor's own tutorial (esp32io.com "ESP32 RS485") says TX -> RXD. **That is
wrong for this board.** We wired it that way, so our signal went into the
module's output pin and the bus never moved. The RXD LED only shows the level on
that pin, which we were driving ourselves. **It was not proof that data reached
the bus.** A buyer review ("TX to TX, RX to RX") gave the answer.

**Fix / correct wiring:**

| ESP32 | Module |
|---|---|
| TX2 (GPIO17) | **TXD** |
| (nothing) | **RXD: leave disconnected** (it outputs 5 V when VCC = 5 V) |
| VIN 5 V | VCC (MAX485 is a 5 V part; it worked on 3V3 at the bench, but 5 V is in spec) |
| GND | GND |

**How to spot it in 2 minutes next time (do this FIRST, before any code):**
1. **Static drive test:** hold the ESP TX pin as a plain GPIO HIGH, then LOW
   (dmx_probe `hold 1` / `hold 0`). Meter A+ and B- to the ground terminal. **A
   working input flips A/B between HIGH and LOW.** If A/B don't change, you're on
   the wrong pin (or the board is dead). We saw A 0 V / B 3 V for both. Idle bias
   is A 4.3 V / B 0 V.
2. **Pattern test:** send all-255 vs all-0. The A/B DC readings must differ.
3. **Loopback:** jumper TX to a spare GPIO and count breaks + bytes (dmx_probe:
   jumper GPIO17 -> GPIO4, `status`). This proves the ESP output is valid DMX, so
   you stop suspecting the code.

**Applies to:** `props/zombie-dmx-light` (the only RS485 node so far). **Any future
RS485 / DMX / Modbus node using an "auto flow control" TTL-RS485 board:** confirm
the label direction with the static drive test before trusting any tutorial.
Boards with explicit DI/DE/RE/RO pins (plain MAX485 breakouts) don't have this
ambiguity.

---

## 2. esp_dmx: DMX_NUM_2 crashes on espressif32@6.12.0 (Arduino 2.0.17)

**Symptom:** a boot loop with `LoadProhibited` in `uart_ll_set_sclk` <-
`dmx_uart_init` <- `dmx_driver_install`.

**Cause:** esp_dmx `src/dmx/hal/uart.c` sizes its UART table with
`#if DMX_NUM_MAX > 2`. `DMX_NUM_MAX` is an enum, so the preprocessor sees 0 and
UART2's `dev` pointer is never set (NULL).

**Fix:** use `DMX_NUM_1` only. For a second UART (e.g. a loopback receiver) use
plain `Serial2`. **Applies to:** any esp_dmx use on this platform pin.

---

## 3. DMX fixture probing: master channel first

Cheap 7-ch pars (master, R, G, B, W, mode, strobe) show **nothing** when you raise
a single channel, because the master dimmer (start address +0) is 0. That looks
exactly like "no DMX". **Probe in pairs: master = 255 + one channel at a time.**
Test whether DMX works at all with **all slots 255**. Also press **ENTER** on the
`d.xxx` menu to arm DMX mode.

---

## 4. Photosensitive strobe limit: <= 3 flashes/second

The WCAG / Ofcom guideline is **no more than 3 flashes per second**. The danger
zone is roughly 5-30/s, peaking around 15-20. A 10 Hz white strobe on the bench
was painful to look at. The zombie DMX strobe now uses 50 ms flashes spaced
350-600 ms apart (<= 2.9/s) at half brightness, and Joe said it was "less
upsetting". Red is the most provocative flashing colour. Post a "strobe lights in
use" sign regardless. **Applies to:** every strobe effect in the show (DMX, IR,
Eave, etc.).

---

## 5. ESP-01S USB adapter (CH340, PROG switch): ONE esptool connection per plug

In PROG mode the first esptool connect works. A second connect (e.g. `chip_id`
then `write_flash`) fails with "Timed out waiting for packet header" or "Serial
data stream stopped". **Fix:** replug in PROG, then do a single connection with
`--before no_reset --after no_reset` that reads the MAC and writes in the same
session. Check the MAC is not a deployed board first. Switch to UART + replug to
run. **Applies to:** `props/zombie-ir-lights`, `props/zombie-relay-esp01s`, and any
ESP-01S on this adapter.

---

## 6. Opening the ESP32 DevKit serial port resets the board (CP2102 DTR/RTS)

Each new serial connection reboots the classic DevKit, so RAM state (last
captured IR code, a running effect) is lost. **Design bench tools to be
stateless:** commands carry their arguments (e.g. `n<hex>`, `a 255`). Also, junk
bytes right after boot can swallow the first command; send a newline and flush
the input before the first command. **Applies to:** the ESP32 DevKit
`AA:BB:CC:DD:EE:FF` and any CP2102 DevKit.

---

## 7. 44-key IR LED controller (the Zombie Container IR light)

- **Aim:** a Dorhea IR TX module has a ~20 degree beam and ~1.3 m range at 5 V.
  Off-angle looks exactly like a dead module (3 "bad" modules were aim).
- **Command spacing:** the controller drops FLASH/QUICK when they're sent tighter
  than the proven W sequence (WHITE, BRIGHT UP x10, FLASH, QUICK x10 at
  150/300/60 ms spacing). The symptom is solid white instead of strobe.
- **POWER is a toggle:** never automate it. Use DIY1 mixed to black (0,0,0) as an
  idempotent "off"; it survives a power cycle.
- Details: `props/zombie-ir-lights/README.md`, `props/ir-learner/codes.md`.

---

## 6C:C8:40 DevKits: live serial repeats line tails (2026-10-01)

**Correction (same day):** NOT tree ground noise. Patch Right (AA:BB:CC:DD:EE:FF) on the Workshop's
own USB did the same, at 534 KB/s = 46x what 115200 baud can carry, so the bytes never crossed the wire:
the board's USB-serial chip / host re-delivers its last buffer. Nothing reads USB during a show; use the
on-chip black box (`tools/blackbox_pull.sh`), never a live monitor, for these boards. Original note:


With the tree's main power ON, the bridge's live serial from the Tree repeats the tail of the
last line it sent thousands of times whenever the Tree goes quiet (one boot line ~76,000x in
2 min). The Tree's own black-box log (on the chip) has each line once, so the duplication is on
the USB/serial link (noise on the tree's ground/USB cable), not the firmware. Same root as the
09-29 "garbage with tree power on". Flashing worked anyway (4/4 hash-verified). Read the Tree
with `tools/blackbox_pull.sh tree` (collapses repeats), not a live monitor. Also: a pyserial open
from the bridge did NOT reboot the Tree's DevKit on 10-01 (§6 applies to some hosts/tools;
esptool's RTS reset does reboot it).

## 360° "MG90S" looks identical to a 180° one (2026-10-01, shoulder crows)
A replacement MG90S for the head crow spun round and round on SWEEP: it was a **continuous-rotation**
servo (the signal sets speed + direction, 90 = stop; no position). No firmware can make it hold an
angle. **Before fitting any servo: turn the horn by hand, unpowered — a 180° servo hits a hard stop at
each end, a 360° one keeps turning.** The blue SG90 that replaced it is 180°.

## Mounted servo buzzes = binding, cut power (2026-10-01, shoulder crows)
A servo buzzing while holding = it can't reach the commanded angle (something blocks it) and draws
stall current (the 09-29 shoulder-crow servo that melted the VIN wire). `SERVOS OFF` first, then
find the real limits with `tools/servo_jog.py` (arrow-key jog via the firmware's `POS`, marks
LOW/HIGH) instead of guessing SWEEP ranges.

## Workshop multi-slot card reader: never `udisksctl power-off` (2026-10-01)
`udisksctl power-off -b /dev/sdg` (the crow card) powered off the **whole USB reader**: every slot
(sdd–sdi) vanished until the reader was unplugged and replugged. Unmount only (`udisksctl unmount`),
then pull the card.

## Line-tail repeat flood also on AA:BB:CC:DD:EE:FF (2026-10-01)
The §6C:C8:40 symptom (live serial re-delivering a line tail at far over 115200 baud) also hit the
shoulder-crow board on the Workshop's own USB, so it isn't one board batch. Bench reads: open the port
once, read in real time, keep only whole `[`-prefixed lines, de-duplicate.

## 5 V buck on VIN + USB at the same time = USB back-feeds the buck, board dies silently (2026-10-02, Singing Pumpkins)

**Part:** Joe's go-to "5V Buck Converter Module, DC 5-30V to 5V Step-Down, 1.8A" (Amazon 10-pack), used
on many props: 12 V in, 5 V out to the ESP32 VIN + the prop's modules, grounds tied after the buck.

**Symptom:** ESP32 on Workshop USB for a bench test, prop 12 V **OFF**, buck output still wired to VIN.
The board went completely silent: no app output, nothing at 74880, **esptool "No serial data received"**
even after an RTS hard reset, while the CP2102 still enumerated (/dev/ttyUSB0 present). Looks exactly
like a dead ESP32. Connecting the 5 V to VIN with USB in made the board's LED go **out**.

**Cause (A/B confirmed 11:30-11:42):** USB 5 V flows backwards out of VIN into the unpowered buck's output
and everything else on that 5 V rail (IR module, serial module), and the buck loads its own output when its
input is dead. USB sags until the ESP32 browns out / never starts. Pulling a ground (breaking the loop) =
board back instantly; **VIN disconnected + 12 V on + USB = stable through the whole relay test.**

**Rule:** never USB and the buck on VIN at the same time on a prop with 12 V off.
- **Bench (USB):** unplug the buck's 5 V from VIN (insulate it), keep ALL grounds tied (signals need them),
  12 V on so the buck still feeds the other modules.
- **Yard:** buck 5 V -> VIN, no USB.
- **Spot it fast:** "dead" board + esptool can't connect + a buck on VIN -> disconnect VIN before anything else.

**Applies to:** every prop with this buck on the ESP32's VIN (Joe uses it widely). Confirmed on the
Singing Pumpkins (AA:BB:CC:DD:EE:FF). **Open question for the Tree/Pumpkin Men/Patch MP3 props:** their bench
rule is "flash and log with main power OFF" (docs/mp3-yx5300-driver.md) and on 09-29 the Tree ran on bridge USB
with its power off, so USB back-fed its 5 V 3 A buck there too without killing the board (different buck?).
Their "garbage serial with main power ON" (USB + buck both on the rail, plus a ground loop through the USB
cable) may be the same family. Not tested: check which buck each prop uses before changing those rules.

**Related on the same board:** the HW-027 serial module must be powered from the ESP32's **3V3**, never the
buck's 5 V (its TTL TX would drive 5 V into GPIO21; sdcard README wiring section).

**Caught again 2026-10-20 (Singing Pumpkins, bridge Pi):** Pi USB plugged into the SP ESP32 with the stake cord
(prop 12 V) OFF and the buck still on VIN. Symptom this time: the **Pi reported under-voltage** (`vcgencmd
get_throttled` 0x50005) -- it was feeding the prop's modules backwards through the ESP32. Joe unplugged VIN + GND
before anything was opened; flash then clean. Check `get_throttled` on the Pi as a quick back-feed tell.

## ESP32 DevKit: D22 / TX0 / RX0 / D21 sit side by side (2026-10-02, Singing Pumpkins)
On the 30-pin DevKit row `D23, D22, TX0, RX0, D21` a UART pair shifted one pin lands on **TX0/RX0 = GPIO1/3, the
USB console**. Symptom: 0 bytes from the device, and the device receives the ESP32's own log text (the BrightSign
answered `ERR unknown command: [00:00.395] ... WiFi up`). Check the pin labels before anything else. The same
day's `-> LIST` x hundreds at one millisecond on AA:BB:CC:DD:EE:FF was the line-tail repeat artifact above, not traffic.

Repeat 2026-10-04 (Stain Glass board): a wire meant for D22 landed on TX0 -> endless reset loop + the ESP's own help text
echoed back on the console. Moving it fixed it; the board needed one RTS reset to come out of the loop.

## SD card reader drops mid-write (2026-10-02)
Workshop USB reader Genesys `05e3:0748` (showed as 5 LUNs sdd-sdi) disconnected/reset during a 600 MB copy to the Scarecrow card: kernel `Media removed` then `Sense Key: Aborted Command / Incompatible medium installed` on every sector, `cp` stuck in D state, card left half-written with a dirty FAT. Retired for card writes; the new reader `1908:0226` wrote + verified clean. If a card write throws I/O errors: stop, swap readers, **reformat** (`flash_a1s_card.sh` format mode) rather than copy over a dirty FAT, then md5-verify every file.

Repeat 2026-10-04 (STAINGLASS card, 14 MB, forced to use the Genesys reader): reader re-enumerated right after the
copy (sdg -> sdf), `Volume was not properly unmounted`. Files were intact (md5 from the fresh mount); `fsck.fat -n`
showed ONLY the dirty bit (boot-sector offset 65 differs from backup = that bit). Recovery without reformat:
`udisksctl unmount` -> `fsck.fat -n` (must show no FAT/cluster errors) -> `fsck.fat -a` -> remount -> md5 every file
-> unmount. If `-n` shows anything beyond the dirty bit, reformat instead.

## MOSFET LED channels glow faintly at 0 while the ESP32 is on PC USB (2026-10-07, parkway-mosfet)
ESP32 on Workshop USB driving 4 single Ceksezx MOSFET modules, 12 V on the LED side, grounds tied. With every
output at 0 (`fx off`, status `R0 G0 B0 A0`) **all four colors still glowed faintly**; during swamp the unused
red/amber glowed too. Moving the ESP32 off USB onto the prop's AC/DC converter (VIN) **fixed it completely** (Joe).
Cause: the PC's USB ground vs the 12 V supply's ground (ground loop / offset), not firmware and not module leakage.
Rule: judge LED darkness/color **only on the prop's own supply**, never while on PC USB. And never connect USB
and the converter on VIN at the same time (a 4-ch MOSFET board + an ESP32 blew in this project; see the buck
back-feed entry above). If it ever glows on its own supply: 1k-4.7k bleeder across each LED channel, check signal GND.


## Bridge flash: verify the bins ON the Pi (2026-10-27)
The Pi lost power ~3 min after an scp; the staged bins were zero-filled (ext4 delayed allocation). esptool flashed the zeros and still printed "Hash of data verified" because it compares chip to file, not file to build. The Tree would not boot until reflashed. Always `sync` after scp and compare the Pi-side `md5sum` to the build before `write_flash`.

## BrightSign reads the SD card only at power-up (2026-10-27, Front Door)
Twice in one session a freshly written calibration card went into the door XT1143 while it was running, and Joe's photos
still showed the PREVIOUS clip (labels from the old round) - the player kept looping what it had loaded, even with the card
pulled. Tell: no `boot_ok.txt` on the card when it comes back (every autorun writes it at PlayFile). Rule: player power
OFF -> card in -> power ON, and check the on-screen labels/pattern before photographing. Applies to every BrightSign prop
(door, window 4K1142, Singing Pumpkins XT1143).

## 6. Cut USB cables: wire colors are NOT a polarity standard — meter before connecting (2026-10-28)

The Spooky Tree's ESP32 (AA:BB:CC:DD:EE:FF) was killed on 10-28 by a cut micro-USB cable
wired from a charger. The cable had only **red + white** inside; red was assumed +5 V and
white GND. That was reversed, and the reversed 5 V killed the board.

- The USB spec colors are red = VBUS, black = GND, white = D-, green = D+. A 2-wire
  charge-only cable has no required colors, and cheap ones use whatever wire is on the spool.
- **Rule:** never trust colors on a cut cable. Plug it into the charger and meter it
  (red probe on one wire, black on the other: +5 V = correct, -5 V = swapped) before it
  touches any board.
- Better: don't cut anything. Use an intact USB cable from the charger into the ESP32's own
  micro-USB port. This is what the 10-28 "USB power" plan meant.
- A reversed feed can also push current through anything sharing that ground (on the Tree,
  the ESP-MP3 GND wire), so check the YX5300 and its SD card after an event like this.
- **That cable, measured after the fact (Joe 10-28): RED = NEGATIVE, WHITE = +5 V** — the
  opposite of the guess. Mark it with tape before anyone reuses it.
- Replacement Tree board: AA:BB:CC:DD:EE:FF (`MAC_SPOOKY_TREE` updated, since the firmware halts on a MAC mismatch).
