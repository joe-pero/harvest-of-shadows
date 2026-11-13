# A1S board deep-dive + audio & fleet architecture

> **Read with `docs/CURRENT-BUILD.md` (2026-09-24).** Board, codec, SD, fleet and
> cue plumbing here are the hardware reference. What each segment plays (INTRO /
> MIDDLE / HOLD / OUTRO content, MIDDLE delivery timing) follows CURRENT-BUILD;
> references below to `docs/_history/dialogue-architecture.md` are historical.

**Status: DESIGN — 2026-09-06, revised 2026-09-06 (A1S confirmed as fleet
leader/bridge; Main Switch demoted to fallback; hardening decisions locked).
Bench 2026-09-06 — Steps 1-5 firmware built + validated on the board with
placeholder audio. CONFIRMED: codec (ES8388), PSRAM (4 MB), chip rev, DIP/GPIO
truth table (Part 0, §1.2, §1.3), **and the WiFi-STA + ESP-NOW coexistence
go/no-go on ch 11 — PASS, leader role is viable (§3.4 H3)**. Still pending real
hardware: SD card mount (no card yet), audible audio check (needs a speaker),
and the live Scarecrow↔Burt handshake SUCCESS path (needs Burt's board powered).
Firmware in `props/scarecrow-narrator-a1s/` (env `scarecrow`); bench logs in
`props/scarecrow-narrator-a1s/bench/`.**

This maps the ESP32-A1S narrator board completely, specifies how the INTRO /
MIDDLE / OUTRO audio is stored and delivered, and specifies how the per-cycle
theme+tone reaches the whole ESP-NOW fleet. It is the layer under
`docs/a1s-bringup-test.md` (the throwaway bring-up sketch) and
`docs/_history/dialogue-architecture.md` (the dialogue seam design).

Applies to both narrator nodes — **Scarecrow Narrator A1S** (`NODE_SCARECROW_A1S`
`0x06`, MAC `AA:BB:CC:DD:EE:FF`) and **Skeleton Narrator A1S**
(`NODE_SKELETON_A1S` `0x07`, MAC `AA:BB:CC:DD:EE:FF`) — same board, same base
firmware, differing by NodeID, character, and SD content set. **The Scarecrow
A1S additionally runs the fleet-leader / ESP-NOW-bridge role** (Part 3) — that is

> **MACs bench-confirmed 2026-09-06** — read directly off both chips
> (`esptool read-mac` + firmware `esp_read_mac(ESP_MAC_WIFI_STA)`):
> Scarecrow = `AA:BB:CC:DD:EE:FF`, Burt = `AA:BB:CC:DD:EE:FF`. Both match the
> `ProjectConstants.h` constants exactly — neither was stale. Both boards are
> ESP32-D0WD-V3 rev 3.1, 4 MB flash, 4.00 MB PSRAM, **ES8388** codec @ I²C 0x10,
> DIP 1,2,3-ON (working 1-bit SD). Electrically interchangeable.
the only firmware difference of substance between the two.

### Architecture decision — confirmed 2026-09-06

**The Scarecrow A1S is the show's primary authority and the WiFi→ESP-NOW
bridge.** It receives the theme+tone decision (and the pre-show / main-show cues)
from the Python orchestration layer on detect-server over WiFi, then originates
the ESP-NOW broadcast to the rest of the fleet and the narrator chain.

- **Main Switch Controller is demoted to fallback-only** — manual tone button +
  PIR backup trigger, used only when the A1S / Python path is down. It is no
  longer the show's primary authority or the Linux↔mesh relay.
- **Eave Coordinator is untouched** — it stays on lighting + calendar duty only.
  No conflict: Eave's traffic (6-byte `CmdPacket` to Parkway/Area lights,
  calendar replies) and the A1S's traffic (show `Msg` bursts, narrator chain) are
  disjoint command sets on the same channel-11 broadcast medium; see §3.3.
- **Theme-deck authority is Python** (`orchestration/showsim/theme_deck.py`).
  The `nextTheme()` deck in Main Switch firmware is now **vestigial** — it only
  runs if Main Switch is ever operating fully standalone as the fallback path,
  never during normal operation.

---

## Part 0 — what is and isn't verified here

| Verified | How |
|---|---|
| ESP32-A1S module, AI-Thinker "ESP32 Audio Kit V2.2" carrier, silkscreen `A541` | Joe's physical description |
| **Both audio-side USB connectors are micro-USB** (UART + power) — *not* USB-C | Joe's physical photo, 2026-09-06 (corrects the earlier assumption) |
| Pin map below (I2S, I2C, SD, PA, keys, LEDs) | Cross-checked across esp-adf board defs, NuttX board port, pschatzmann arduino-audiokit, community pinouts — they agree; I²C + KEY/SD lines now also bench-measured (§1.2, §1.3) |
| **SoC = ESP32-D0WD-V3, silicon rev v3.1**, dual-core 240 MHz, 4 MB flash | Bench 2026-09-06 — `esptool chip-id` + Arduino `ESP.getChipModel()` |
| **PSRAM present and usable — 4.00 MB** (4 192 123 B, ~4.13 MB max single alloc) | Bench 2026-09-06 — `psramFound()` / `ESP.getPsramSize()` with `-DBOARD_HAS_PSRAM`. The MIDDLE push-then-play buffer (§2.2) is comfortably covered. |
| **Codec on THIS board = ES8388** (I²C `0x10`, answers at 100 kHz and 400 kHz; nothing at `0x1A`) | Bench 2026-09-06 — I²C scan on SDA 33 / SCL 32. The bring-up doc's ES8388 assumption was correct. AC101 path not needed. |
| **This board's MAC = `AA:BB:CC:DD:EE:FF` → it is the Scarecrow Narrator A1S** (`NODE_SCARECROW_A1S` 0x06, the fleet leader) | Bench 2026-09-06 — `esp_read_mac()` matches `MAC_SCARECROW_A1S` in `ProjectConstants.h` |
| **DIP bank as shipped = 1,2,3 ON / 4,5 OFF.** Electrically connects SD `CLK`(GPIO14), `CMD`(GPIO15), `D0`(GPIO2), `D1`(GPIO4), `D3`(GPIO13) to the socket; isolates `D2`(GPIO12). | Bench 2026-09-06 — GPIO float/pull-up/pull-down probe (§1.2). This is *better* than the doc's original "3/4/5 only" target: it already gives a clean 1-bit bus **plus D3's pull-up** (which the ESP-IDF SDMMC driver wants) while keeping the GPIO12 boot-strap unloaded. |

| **NOT** verified — still open | Why |
|---|---|
| **WiFi STA + ESP-NOW TX coexistence — CONFIRMED PASS 2026-09-06** | Bench `fleet_coex_test()`: associated to "<your-ssid>" on **channel 11** (IP 192.168.1.x, RSSI ~-48), fired **60/60 ESP-NOW broadcasts** while pulling a **200 KB HTTP GET (200, full body)** — association held, channel stayed 11, RSSI stable. Repeated **with I2S playback active** — same PASS. **The leader role is viable on this specific A1S; the dedicated mesh-gateway fallback (§3.4 H3) is not needed.** Log: `bench/step4-5_wifi_espnow_integration_2026-09-06.md`. |
| **Scarecrow↔Burt live handshake — CONFIRMED 2026-09-06, both directions, SUCCESS path** | Two real boards, Scarecrow on PC + Burt on a wall charger, both on ch 11. 3/3 cycles each way: Scarecrow→Burt unicast `CMD_NARRATOR_2_START` → native `ESP_NOW_SEND_SUCCESS` **on the 1st attempt** (`cue ok=3 fail=0, avg 1.0 tries`); Burt→Scarecrow `CMD_ACK` "starting" → native SUCCESS 1st attempt, **received by Scarecrow** (`burt-acks-rx=3`). Burt latched theme/tone off both the broadcast burst and the cue payload; 63 broadcast frames received clean. Neither side ever blocked. In practice Burt acts on the *broadcast* copy of the cue (arrives mid-burst, before the unicast) and the unicast confirms — the belt-and-suspenders design working as intended. Logs: `props/scarecrow-narrator-a1s/bench/live_handshake_scarecrow-side_2026-09-06.md`, `props/skeleton-narrator-a1s/bench/live_handshake_burt-side_2026-09-06.md`. |

| **NOT** verified — still open | Why |
|---|---|
| **SD card actually mounts** | No microSD card on hand yet (render job in progress). The probe fails with `send_op_cond → 0x107` (bus timeout) in both 1-bit and 4-bit — the signature of an **empty slot**, not a mux/signal fault (a wrong-DIP card gives CRC/pull-up errors instead). Re-run §1.6 SD line with a FAT32 card inserted; expected to pass in 1-bit mode at the current DIP setting. |
| **Audible audio** | ES8388 init + I2S clock tree + test-tone streaming all verified in firmware, but nothing was connected to LINE OUT / the headphone jack to *hear* it. Needs a powered speaker + PicoTalk on the bench. |
| 3.3 V rail behaviour under WiFi TX + playback | Scope/ADC measurement not done (§1.4 quirk 4). No brown-out *observed* across the coexistence + cycle runs (incl. Burt on a wall charger), but not instrumented. |
| Main-show OUTRO cue path (`CMD_START_SCENE` → Burt `mainshow=1`) | Burt's code handles it; the Scarecrow cycle-sim only runs the pre-show flow, so `mainshow=1` wasn't exercised live. Same handshake mechanism — low risk. |

**Remaining bench actions:** §1.6 SD line (needs a card); audible check (needs a
speaker); the live 2-node handshake (needs Burt). Codec, PSRAM, DIP/GPIO, and the
leader-role coexistence gate are all confirmed.

---

## Part 1 — board characterisation

### 1.1 Module, codec, and the audio data path

- **SoC:** ESP32-A1S = ESP32-D0WD (dual-core LX6, 240 MHz) + 4 MB flash +
  **≈4 MB usable PSRAM** (8 MB physical on most modules; the standard SDK config
  exposes ~4 MB without `himem`). The PSRAM is the reason this board can buffer a
  whole audio file in RAM — see Part 2.
- **Audio codec:** ES8388 (recent batches) **or** AC101 (older, EOL). Same
  carrier PCB, no silkscreen tell. Identify by I²C address (`0x10` = ES8388,
  `0x1A` = AC101) or by reading the chip marking next to the 3.5 mm jacks.
  Firmware must either be built for the known part or probe at boot and pick the
  right register init. **The ESP32-audioKit / arduino-audiokit HAL abstracts
  both** — use it rather than hand-rolling codec register writes.
- **Control bus (I²C):** `SDA = GPIO33`, `SCL = GPIO32`. Codec setup only —
  sample rate, routing (DAC → line-out / headphone / Class-D amp), volume, ADC
  gain. Written once at start-up and on volume changes.
- **Audio bus (I²S, codec slave / ESP master):**
  | Signal | GPIO |
  |---|---|
  | MCLK (codec master clock) | `GPIO0` — **also the BOOT strap**; see quirks |
  | BCLK / SCLK (bit clock) | `GPIO27` |
  | LRCK / WS (word select) | `GPIO25` |
  | DOUT (ESP → codec, playback) | `GPIO26` |
  | DIN (codec → ESP, mic/line-in) | `GPIO35` — input-only pin |
- **Power amp enable:** `GPIO21` (aka `CTRL`/`PA`/`SD_MODE` of the on-board
  NS4150 Class-D). **Drive HIGH for the on-board 3 W speaker amp; LOW disables
  it.** The narrator does **not** use the on-board amp — it feeds a *powered*
  speaker + the PicoTalk from **LINE OUT (LOUT/ROUT)** or the earphone jack,
  which are post-DAC and independent of the Class-D stage. So: **hold GPIO21 LOW**
  — lower current draw, less switching noise on the line the PicoTalk is
  level-following. (The bring-up doc's "drive GPIO21 HIGH or there's no sound"
  note applies only if you're using the on-board speaker terminals; supersede it
  for the real node.)
- **Mics:** two on-board analog mics into the codec ADC. AI-Thinker shipped many
  V2.2 boards needing a capacitor rework for the mics to function. **Irrelevant
  to the narrator** — it never records — so don't spend time on it.

### 1.2 The 5-position DIP switch

**What it does:** the microSD slot and the JTAG header share ESP32 pins
`GPIO2, GPIO12, GPIO13, GPIO14, GPIO15`. The DIP bank is a set of series switches
that connect (or isolate) each shared line to the microSD card slot. The
silkscreen is notoriously unhelpful (positions are labelled with pin names like
`IO13` / `IO15`, not "SD" / "JTAG").

**Consensus on the mapping** (verify polarity on the actual unit — see below):

| Switch | Connects when engaged | ESP32 pin | SD role |
|---|---|---|---|
| 1 | SD DATA2 | `GPIO12` | 4-bit data line — **also a boot strap** (flash voltage) |
| 2 | SD DATA3 / CS | `GPIO13` | 4-bit data line / SPI-CS — **also KEY2** |
| 3 | SD CMD / MOSI | `GPIO15` | command line — **also JTAG MTDO** |
| 4 | SD CLK / SCK | `GPIO14` | clock |
| 5 | SD DATA0 / MISO | `GPIO2` | data line 0 — **also a boot strap** |

- **All engaged →** full 4-bit SDMMC to the card. KEY2 is dead, GPIO12/GPIO2
  straps are loaded by the card, JTAG unavailable.
- **All disengaged →** those five pins free for GPIO / JTAG, **no SD card**.
- Community sources disagree on whether "ON" means toward or away from the
  numbers, and at least one revision defaults to "SD works with all switches
  OFF". **This is exactly why the board has a finicky reputation** — do not trust
  a table, test it.

**What the narrator node needs:** the SD card is **essential** (INTRO/OUTRO pools
live on it — Part 2). The KEY buttons are **not** needed (no operator UI on the
prop).

---

#### BENCH-CONFIRMED 2026-09-06 — DIP bank as shipped: **1,2,3 ON / 4,5 OFF**

Joe's physical read of the switch bank on the connected board: switches **1, 2,
3 = ON**, switches **4, 5 = OFF** (the AI-Thinker out-of-box default — not yet
deliberately set).

**What that configuration actually does, measured (not a forum guess):** a
throwaway characterization sketch
(`props/scarecrow-narrator-a1s/src/main.cpp` at this commit;
raw log `props/scarecrow-narrator-a1s/bench/characterize_2026-09-06_dip-123ON.md`)
probed each shared pin three ways — floating, internal pull-up, internal
pull-down. A pin that stays HIGH under an internal pull-*down* is being held by
an **external** pull-up, i.e. it is electrically tied to the microSD socket
(AI-Thinker populates ~10 k pull-ups on the socket's DAT/CMD lines). A pin that
follows the internal pull is **isolated** from the socket by its DIP switch.

| ESP32 pin | SD signal | Measured (float / pu / pd) | State in 1,2,3-ON config |
|---|---|---|---|
| GPIO14 | CLK | 1 / 1 / 1 | **connected to socket** |
| GPIO15 | CMD (SPI MOSI) | 1 / 1 / 1 | **connected to socket** |
| GPIO2  | DATA0 (SPI MISO) | 1 / 1 / 1 | **connected to socket** |
| GPIO4  | DATA1 | 1 / 1 / 1 | **connected to socket** |
| GPIO13 | DATA3 (SPI CS) / KEY2 | 1 / 1 / 1 | **connected to socket** — carries the DAT3 pull-up the SDMMC driver checks for |
| GPIO12 | DATA2 / strap MTDI | 0 / 1 / 0 | **isolated** — no socket pull-up reaches it; free, and the boot strap is NOT loaded by a card |

**Verdict — the shipped default is a good target; keep it.** It already gives a
complete **1-bit SDMMC bus** (CLK + CMD + D0) *plus* the DATA3 line and its
pull-up (which stops the IDF `sdmmc` driver from erroring on "missing DAT3
pull-up"), while leaving GPIO12 — a boot strap — disconnected from the card. That
is strictly better than this doc's original "engage only 3/4/5" idea, which
would have isolated GPIO13/DAT3 and tripped that driver check. The only thing the
default costs is GPIO12 for 4-bit mode, and **4-bit is not wanted here** (§2.1: a
1 MB WAV loads faster than it plays even at 1-bit; 4-bit would also re-load the
GPIO12 strap through the card — §1.4 quirk 3).

- **Firmware SD config:** 1-bit mode, `SD_MMC.setPins(14 /*CLK*/, 15 /*CMD*/,
  2 /*D0*/)` then `SD_MMC.begin("/sdcard", /*mode1bit=*/true, false,
  SDMMC_FREQ_DEFAULT)`. Do **not** call the 4-bit `setPins` overload.
- **Do NOT flip any switch.** No change needed to reach the intended config —
  the board shipped in it.
- **Schematic cross-reference note:** community DIP-switch tables (pschatzmann,
  esp-adf, radio-bastler) disagree on switch→pin order and ON/OFF polarity
  across board revisions, which is exactly why this was measured electrically
  instead. The measurement *is* the schematic-authoritative answer for this
  specific unit. The AI-Thinker datasheet's own framing ("the DIP switches are
  for choosing SD-drive vs JTAG; the encoder switch selects IO13/IO15
  functions") is consistent with what was measured: in the shipped position the
  SD path wins and JTAG is unavailable.
- **Card mounting — now MEASURED** (2026-09-08/09, see "Card mount" below):
  Scarecrow mounts clean 1-bit at this DIP setting; Burt mounts too but **won't
  flash with the card in** (Burt-only gotcha, below).
- **If a future need for 4-bit ever appears:** flip switch **4** (the D2/GPIO12
  line — deduced: it's one of the two currently-OFF switches, and switch 5 pairs
  with a line already seen connected) to ON and re-run the sketch to confirm
  GPIO12 picks up the socket pull-up. Not needed for the narrator.

### Card mount — MEASURED 2026-09-08/09 (and a Burt-only flash gotcha)

Both boards now run with a real microSD (`pool_version 20260908`).

- **Scarecrow** (`AA:BB:CC:DD:EE:FF`): card mounts clean in 1-bit at the shipped
  DIP setting — `SD mounted: 59.48 GB, type 3`, `index.csv` parses, all pools
  (`intro/outro/hold` = 50 × 6, `bridge` = 30) load. **Flashes and boots fine
  with the card inserted.** ✅
- **Burt** (`AA:BB:CC:DD:EE:FF`): **⚠ will NOT flash — and does not boot — with
  its microSD card inserted.** esptool reaches the ROM (reads MAC) and runs the
  stub, then `Failed to communicate with the flash chip`; fails identically with
  `--no-stub` and at 115200/460800 baud; the old firmware emits zero serial.
  **Pull the card → flashes first try and boots normally.** Same cable/port that
  flashes Scarecrow; DIPs confirmed 1,2,3 ON / 4,5 OFF.
  - **Probable cause:** on this unit an inserted card *is* loading **GPIO12
    (DATA2 = the VDD_SDIO flash-voltage strap)** at reset — 3.3 V flash then runs
    at 1.8 V and reads back as noise. That contradicts the isolation measured
    above, so on Burt specifically either **DIP-1's contact is marginal**, or
    **this card** bridges DATA2 where Scarecrow's doesn't.
  - **Workaround (historical):** pull Burt's card before every flash.
  - **RESOLVED 2026-09-19, per Joe:** it was the old microSD card itself —
    confirms the second hypothesis above ("this card bridges DATA2 where
    Scarecrow's doesn't"), not the DIP-1-marginal-contact theory. Replaced;
    no longer requires pulling the card before a flash. Not independently
    re-verified across several power cycles this session.

---

**Original bench procedure (superseded — all steps done):**
1. ~~Photograph the switch bank; note which way each toggle points.~~ Done: 1,2,3 ON / 4,5 OFF.
2. ~~Flash a scanner: I²C-scan, `SD_MMC.begin` 1-bit, read KEY GPIO idle levels.~~ Done — see log.
3. ~~Insert a FAT32 card, re-run, confirm 1-bit mount + card size.~~ Done 2026-09-08/09
   (Scarecrow `59.48 GB` clean; Burt mounts but has the flash-with-card-in gotcha — "Card mount" above).

### 1.3 GPIO budget — consumed vs genuinely free

**Consumed by the audio + SD subsystem (target config: ES8388 + 1-bit SD):**

| GPIO | Use | Notes |
|---|---|---|
| 0 | I²S MCLK | + BOOT strap |
| 27, 25, 26 | I²S BCLK / LRCK / DOUT | |
| 35 | I²S DIN | input-only; unused by narrator but wired |
| 32, 33 | I²C SCL / SDA to codec | |
| 21 | PA enable | held LOW by the narrator, but reserved — **don't repurpose** |
| 14, 15, 2 | SD CLK / CMD / D0 (1-bit) | |
| 34 | SD card-detect | input-only; optional to read |
| 39 | headphone-jack detect | input-only; optional |
| 36 (KEY1) | — | input-only; free but can't drive anything |

**Genuinely free and output-capable** — bench-measured 2026-09-06 (float /
pull-up / pull-down on each; "FREE" = follows the internal pull, no external
driver/pull):

| GPIO | Normally | Bench result | Use for the prop |
|---|---|---|---|
| **22** | LED4 (on-board green) | FREE (0 / 1 / 0) | safest free pin — LED eyes or a servo |
| **23** | KEY4 | FREE (0 / 1 / 0) | servo / LED |
| **18** | KEY5 | FREE (0 / 1 / 0) | servo / LED |
| **5**  | KEY6 | FREE (0 / 1 / 0) | servo / LED — `GPIO5` is a mild boot strap (must not be *externally* held low at boot); fine as an output |
| **19** | KEY3 | has a **~10 k external pull-up** (1 / 1 / 1) — AI-Thinker populated the button pull-up | usable as an output or active-low input; just idles HIGH. Fine for an LED; prefer 22/23/18/5 for servos. |
| **12** | SD D2 / strap MTDI | FREE in the shipped DIP config (isolated from the socket) | **available but reserved** — it's a boot strap; only use if 22/23/18/5 aren't enough (they are) |
| ~~13~~ | SD D3 / KEY2 | **NOT free** (1 / 1 / 1) — carries the SD DATA3 line + pull-up in the shipped DIP config | leave alone |
| **36** | KEY1 | input-only, reads HIGH | free but can't drive anything |

**Net:** GPIO **22, 23, 18, 5** are four clean, header-accessible, output-capable
pins — 2× servo PWM + LED eyes + spare, with GPIO19 (idle-high) as a fifth and
GPIO12 held in reserve. GPIO is not a constraint; no SD-shared pin needs to be
touched.

The board's pin header brings out `IO21, IO22, IO19, IO23, IO18, IO5, GND` — i.e.
**IO22/19/23/18/5 are five ready, header-accessible, output-capable GPIOs** (skip
IO21, it's the PA line). That covers 2× servo PWM + LED eyes + spare with no need
to touch the SD-shared pins at all.

**Conclusion:** GPIO is *not* a constraint for this prop. The narrator needs
~3–4 outputs (2 servos, 1–2 LED channels); the KEY header alone provides 5.
Relay control for the "powered-speaker split" mentioned in the brief is also
covered from the same header if it ever lands on this node — though note the
PicoTalk itself is passive (audio-level driven, no GPIO), so the only GPIO the
audio side ever needs is the amp-enable it already has.

### 1.4 Known quirks / gotchas for this board revision

1. **Two codec variants, no silkscreen difference** (§1.1). Wrong codec init =
   board runs, no audio. Probe or build for the known part.
2. **`GPIO0` doubles as I²S MCLK and the BOOT strap.** Auto-reset flashing
   usually works, but a marginal USB-serial adapter or a load on GPIO0 can make
   the board hard to enter download mode — hold BOOT manually if `esptool`
   can't sync. Never wire anything to GPIO0.
3. **`GPIO2` (SD D0) and `GPIO12` (SD D2) are boot straps.** A card in the slot
   plus 4-bit mode occasionally disturbs boot (classic ESP32). 1-bit mode with
   switches 1&2 off avoids the GPIO12 issue entirely.
4. **On-board AMS1117 LDO (5 V → 3.3 V) is marginal under WiFi TX + audio.**
   Brown-outs during WiFi association are the #1 field failure. Feed a clean
   5 V/≥2 A supply, add bulk capacitance at the board, keep `WiFi.setSleep(false)`
   deliberate (needed for ESP-NOW RX latency, costs current).
5. **Audio distorts on loud passages** unless the codec digital volume is backed
   off (~0.7 of full). Normalise the rendered pools to a conservative target and
   set codec volume once.
6. **Headphone-detect (`GPIO39`) reads inverted** (1 = nothing plugged, 0 =
   plugged) and needs pull-up. Only matters if firmware auto-switches
   line-out/headphone routing — the narrator should just hard-route to LINE OUT
   in codec init and ignore the jack.
7. **DIP-switch labelling is by pin name, not function**, and polarity varies by
   revision (§1.2). Assume nothing.
8. **No battery telemetry.** The JST charger has no connection to any GPIO — the
   ESP cannot read battery voltage or charge state. Don't design around battery
   operation (§1.5).
9. **`arduino-audiokit` / esp-adf pin defaults target the "v2.2 / A1S" board** —
   pick that board profile exactly; the LyraT and generic profiles have
   different pins and will silently misbehave.

### 1.5 Power input — bench vs permanent yard install

Both USB connectors on the board are **micro-USB** (confirmed by photo — the
board is *not* USB-C despite the earlier assumption; buy micro-USB cables and
supplies accordingly).

| Input | What it is | Verdict |
|---|---|---|
| micro-USB #1 (UART) | CP2102/CH340 USB-serial — programming + serial monitor + 5 V | Bench only. Use for flashing and bring-up. |
| micro-USB #2 (5 V) | 5 V power in, no data, straight to the on-board regulator | Acceptable for permanent use **if** fed from a good 5 V/2 A adapter, not a phone charger. |
| JST battery | Single Li-ion (3.7 V nom) + on-board charge IC | **Do not use for the installed prop.** No telemetry, finite runtime, and the LDO headroom shrinks as the cell sags. Bench experiments only. |

**Recommended for the yard:** a dedicated regulated **5 V / 2 A** supply into the
power **micro-USB** port (or soldered to the 5 V header pin), separate from the
powered speaker's mains supply, with ~1000 µF bulk cap at the board. The narrator
prop sits near the Scarecrow at the head of the path — within range of the
front-yard ch-11 repeater per `a1s-bringup-test.md`; confirm RSSI at the final
mounting spot before committing the cable run.

**Shopping notes (connector-relevant):** a good **micro-USB** cable (24 AWG power
pair, short) + a 5 V/2 A wall adapter; a spare micro-USB data cable for flashing.
Micro-USB connectors are mechanically weak for a permanent install — strain-
relieve the cable and consider soldering 5 V/GND to the header for the final
mount rather than relying on the micro-USB jack long-term.

### 1.6 Bench bring-up checklist (fills in Part 0's unknowns)

- [x] Photograph DIP bank; record toggle directions. → **1,2,3 ON / 4,5 OFF** (2026-09-06)
- [x] I²C scan → codec ID. → **ES8388 at 0x10** (2026-09-06)
- [x] `ESP.getPsramSize()` / `getFlashChipSize()`. → **PSRAM 4.00 MB, flash 4 MB, ESP32-D0WD-V3 rev 3.1** (2026-09-06)
- [ ] `SD_MMC.begin(..., /*mode1bit=*/true)` with `setPins(14,15,2)` → card
      mounts? card size? **BLOCKED — no microSD card yet.** Probe ran, timed out
      (empty slot). DIP config already correct; just needs a card.
- [x] Read KEY1/3/4/5/6 idle levels. → 22/23/18/5 FREE; 19 has a pull-up; 13 in
      use by SD; 12 free/isolated. Prop I/O comes from the KEY header. (2026-09-06)
- [ ] Play a test tone / WAV → LINE OUT with GPIO21 LOW → confirm audio on the
      powered speaker + PicoTalk jaw movement. **Step 2/3 — firmware building; needs
      speaker + PicoTalk on the bench.**
- [ ] Measure 3.3 V rail during WiFi connect + playback → no brown-out dip. Not
      instrumented; none observed across the Step 4/5 runs.
- [x] **Coexistence test (fleet-leader gate):** ch-11 assoc + `WiFi.channel()==11`
      + 60 ESP-NOW broadcasts + a concurrent 200 KB HTTP GET, with and without
      playback → **PASS 2026-09-06**. `fleet_coex_test()` in firmware; serial
      `x` / `X`. (A second ESP32 logging receipt would further confirm RX; TX +
      assoc + HTTP coexistence is the part that gates the architecture and it
      passed.)
- [x] Paste results into Part 0 / §1.2 / §3.4. (2026-09-06)

**Firmware:** `props/scarecrow-narrator-a1s/` env `scarecrow` —
`es8388.cpp` (codec), `audio_out.cpp` (I2S), `wav.cpp` (PCM stream),
`pools.cpp` (§2.1 SD layout + §2.3 ladder), `middle.cpp` (§2.2 push-then-play),
`fleet.cpp` (WiFi ch11 + ESP-NOW + H1/H1b/H4). Step 1 characterization sketch
archived at `bench/_archived_characterize_sketch.cpp`.

---

## Part 2 — audio storage & delivery

Design target (`docs/_history/dialogue-architecture.md`): **INTRO and OUTRO are large
pre-rendered pools stored on the A1S's own microSD for zero-latency local
playback, no network dependency. MIDDLE is generated live per visitor** and must
arrive over WiFi during INTRO playback.

### 2.1 INTRO + OUTRO on the microSD card

**Volume (per narrator node — each node carries only its own character):**

| | Count | Format | ~Size each | Subtotal |
|---|---|---|---|---|
| INTRO | 50 × 3 themes × 2 tones = **300** | WAV, 24 kHz mono 16-bit, ~10–20 s | ~0.6–1.0 MB | ~250 MB |
| OUTRO | 50 × 3 themes × 2 tones = **300** | WAV, 24 kHz mono 16-bit, ~6–12 s | ~0.4–0.7 MB | ~150 MB |
| Generic bridge (fallback, §2.3) | **30** (Scary, theme-agnostic — one flat pool) | same | | ~15 MB |
| **Total per node** | **~630 files** | | | **≈415 MB** |

Any microSD card handles this with three orders of magnitude to spare. Use an
**8–16 GB industrial/pSLC card** (endurance and cold-tolerance matter more than
capacity for a yard prop). FAT32.

**Storage format decision: keep WAV, not MP3.** MP3 would cut the pool to
~120 MB, but WAV is a zero-CPU, zero-latency decode — the file streams straight
into the I²S DMA buffer and playback starts in <100 ms. Space is a non-issue, so
don't spend the decode budget or the buffer complexity. Optionally render at
**22.05 or 16 kHz mono** (the render pipeline already emits a `--wav16k` copy) —
still fine for a spoken narrator through a yard speaker, and it halves both size
and SD read bandwidth.

**Directory layout — deterministic, never scanned at runtime:**

```
/scarecrow/
  intro/
    corn_scary/     000.wav 001.wav ... 049.wav
    corn_friendly/  000.wav ...
    rotld_scary/    ...
    rotld_friendly/ ...
    pumpkins_scary/    ...
    pumpkins_friendly/ ...
  outro/
    <same 6 buckets>
  bridge/
    generic/        000.wav ... 029.wav     # 30 Scary, theme-agnostic (flat — no buckets)
  index.csv
```

`index.csv` is written by the render pipeline and read **once at boot** into a
small in-RAM table:

```
segment,bucket,count
intro,corn_scary,50
intro,corn_friendly,50
...
outro,pumpkins_friendly,50
bridge,generic,30
```

**Lookup at play time** (no filesystem walk, no blind scan):
1. Firmware already holds the active `(theme, tone)` — for the **Scarecrow A1S**
   (leader) straight from the Python MQTT cue; for the **Skeleton A1S** from the
   Scarecrow's ESP-NOW `SET_THEME`/`SET_MODE` broadcast, re-confirmed in the
   `CMD_NARRATOR_2_START` payload (Part 3).
2. `bucket = theme_tone_string(theme, tone)` → e.g. `"rotld_scary"`.
3. `n = table[segment][bucket]` (from `index.csv`).
4. `i = esp_random() % n`.
5. Open `"/scarecrow/intro/rotld_scary/%03d.wav" % i`. Play.

Selection is O(1) and involves exactly one `fopen`. A missing file (bad card
sector) → increment `i`, retry a few times, then fall through to the bridge pool.

**Anti-repeat (optional polish):** keep the last 3–5 played indices per bucket in
RAM and re-roll if `i` collides — cheap, and stops a lingering visitor hearing
the same INTRO twice. Matches the deck philosophy already used for themes.

**SD read performance:** 1-bit SDMMC on this board sustains well over 1 MB/s; a
1 MB WAV loads faster than it plays. Stream it block-by-block into the DMA buffer
rather than loading whole — playback audibly starts in tens of milliseconds,
which is the "zero-latency local playback" the design calls for.

### 2.2 MIDDLE delivery — **push-then-play, not streaming**

**Recommendation: the A1S fetches the finished MIDDLE file with a single HTTP GET
(the existing `audio_bridge.py :8080` path), buffers the whole file into PSRAM,
verifies it, then plays it. No chunked/real-time streaming.**

Reasoning (the brief asks not to assume streaming is better):

| | Push-then-play (recommended) | Real-time stream |
|---|---|---|
| When can playback start? | Only after Linux finishes vision+LLM+XTTS+loudnorm (~5–20 s per `docs/_history/dialogue-architecture.md`). The transfer (~300–700 KB over ESP32 WiFi ≈ 2–5 s) happens **under the INTRO**, in parallel — the INTRO's 10–20 s runtime hides all of it. | **Same** — the file doesn't exist until TTS completes, so streaming doesn't start any sooner. The bottleneck is generation, not transport. |
| Failure behaviour | Node checks `Content-Length`, downloads fully, optionally checksums, *then* commits. Late/short/corrupt = detected before a single sample plays → clean fallback. | Mid-stream TCP stall / underrun = audible dropout with no recovery path. One retransmit and the jaw stutters. |
| Memory | Whole MIDDLE fits in PSRAM (≈4 MB) many times over. Trivial. | Smaller live buffer, but far more state and edge cases. |
| Complexity | One `HTTPClient` GET, one buffer, one play call. | Ring buffer, watermark logic, reconnect, partial-file handling. |

Streaming would only win if MIDDLE clips were minutes long or had to begin before
they were fully rendered — neither is true here (1–3 sentences, rendered
server-side first).

**Mechanism** (Scarecrow A1S — the leader — on the pre-show cue):
1. **First**, originate the ESP-NOW theme burst (§3.4) — a handful of frames,
   ~tens of ms total.
2. **Then** start the INTRO from SD (§2.1) — local, no radio.
3. **Then** begin fetching the MIDDLE. The URL is deterministic from the session
   ID: `http://192.168.1.x:8080/middle/<session_id>_sc.wav` (`_sc` / `_bu` per
   character). No need to carry the URL in any payload — the session ID is
   already there.

   This ordering is deliberate (§3.4 "sequencing, not tight timing"): the burst
   is off the radio before the sustained HTTP transfer starts, so the one cue
   that matters most for the fleet isn't contending with a multi-second download.
   The time budget is generous — the Skeleton doesn't need its cue until the
   Scarecrow's whole monologue plus the visitor's walk down the path completes —
   so a few tens of ms of deliberate sequencing costs nothing.
4. Linux may not have the MIDDLE file ready yet → the node **polls**: GET every
   ~500 ms, treating `404`/`202` as "not ready", for up to a deadline (see
   below). On the first `200`, download fully into PSRAM.
5. When the INTRO's last DMA buffer drains:
   - MIDDLE fully buffered & valid → ~50–100 ms crossfade into it (per
     `docs/_history/dialogue-architecture.md` "Technical: splicing").
   - not ready → fallback ladder (§2.3).
6. `middle/<id>` files are disposable — a Linux-side cron / the bridge itself
   deletes them after the cycle.

(The Skeleton A1S has no ESP-NOW burst to originate on its cue — it just plays.
Its equivalent MIDDLE fetch, if used, has the entire Scarecrow monologue + walk
as its window, so ordering is a non-issue there.)

**Deadline:** `INTRO_actual_length − 1.0 s` as the soft cap (keep trying while the
INTRO plays), then a hard **+1.5 s grace** of ambient pad before giving up. In
practice the INTRO is 10–20 s and the MIDDLE is ready in 5–20 s, so this succeeds
the large majority of the time; the grace covers the slow tail.

### 2.3 Fallback ladder — what plays when MIDDLE isn't there

Defined, not left open:

| # | Condition | What plays |
|---|---|---|
| 1 | MIDDLE downloaded & valid before the deadline | The live MIDDLE. |
| 2 | MIDDLE still in flight when INTRO ends | Up to **1.5 s** of the ambient bed / a soft pad while the download finishes; then play it if it lands, else → 3. |
| 3 | MIDDLE failed / timed out, SD OK | A **generic pre-recorded "bridge" line** from `/bridge/generic/` — a flat 30-line pool (**Scary, theme-agnostic**) of content-agnostic connective lines that read naturally after *any* open-hook INTRO and before *any* OUTRO, referencing nothing specific ("…warm, and small, and curious, and still walking toward me…"). This is the **primary fallback** — the character keeps talking, no dead air. Scary-only by design: the fallback fires exactly when the vision call that would set the tone is down, so there's no data for a Friendly default — Scary is the majority case, and the Main Switch manual override is the real Friendly safety valve during an outage. |
| 4 | Bridge pool also unreadable (SD fault) | **Silence under the ambient loop.** The yard boombox / area ambient covers the gap. The OUTRO still fires normally at the brick walkway. |
| 5 | Narrator node entirely offline | The mesh show runs without it — `broadcastShowStart()` never waits on any narrator (`docs/_history/dialogue-architecture.md` "the show goes on"). |

The narrator layer can therefore degrade all the way to silence without ever
stalling the show. The only *new* content this requires is the **generic bridge
pool** — **30 Scary theme-agnostic lines per character (60 total)**.
**WRITTEN 2026-09-06** — `orchestration/voice/lines/{scarecrow,burt}/bridge/generic.txt`;
rendering in the same `render_all_production.sh` background job as the main pools.

### 2.4 Impact on the render pipeline

- `bridge/generic.txt` sits beside `intro/` and `outro/` in
  `orchestration/voice/lines/<char>/` — **one flat file, 30 Scary theme-agnostic
  lines** (not split by theme or tone). Seam rules identical to MIDDLE. Rendered
  by `scripts/render_all_production.sh` → `out/pools/<char>/bridge/generic/`.
- The card-provisioning step emits `index.csv` alongside the rendered tree in the
  exact on-card layout of §2.1 (`bridge/generic/NNN.wav`, `bridge,generic,30`),
  so provisioning a card is a straight copy.
- One target loudness (`loudnorm`) across INTRO + OUTRO + bridge + live MIDDLE —
  already the rule in `docs/_history/dialogue-architecture.md`; the A1S sets codec volume once
  and never rides it.
- Card provisioning: `rsync` the rendered `/<char>/` tree to the SD root. Version
  the tree (`index.csv` carries a `pool_version` line) so a node can log which
  pool build it's running.

---

## Part 3 — the A1S as fleet leader & ESP-NOW bridge

> **IMPLEMENTATION STATUS — 2026-09-11:** the Scarecrow-side MQTT cue
> transport described in §3.1/§3.2 below (`harvest/show/params`/`cue`
> subscribe, the `preshow`/`hold`/`mainshow`/`reset` phase dispatch,
> `harvest/show/leader/status` publish) is **built and bench-verified** —
> `props/scarecrow-narrator-a1s/src/mqtt_client.{h,cpp}` +
> `main.cpp`, `BUILD_LOG.md`'s 2026-09-11 entry has the exact test. What's
> below is still the accurate design reference for the wire contract, but the
> "TODO" framing throughout this Part now only applies to the **Python
> publisher side** (detect-server, real `frigate/events` → real publishes) —
> that piece is still exactly the simulator-only state described here. See
> `docs/_history/HANDOFF-2026-09-11-mqtt-cue-bridge.md` for that handoff.

### 3.1 Topology

```
detect-server  (Python orchestration — Show Logic Spec v2)
   │  Frigate MQTT events in → locks (session_id, theme, tone) at the sidewalk
   │  trigger from theme_deck.py (SOLE authority). No firmware deck involved.
   │
   │  MQTT  (broker 192.168.1.x:1883, user halloween)
   │   ├─ harvest/show/params   RETAINED   {session_id,theme,tone,cycle,ts}
   │   └─ harvest/show/cue                 {session_id,phase:"preshow"|"hold"|"mainshow"|"reset"}
   ▼
Scarecrow A1S  (NODE_SCARECROW_A1S 0x06)  ── FLEET LEADER / WiFi→ESP-NOW BRIDGE
   │  always-on WiFi STA on ch 11  +  ESP-NOW  (see §3.4 coexistence)
   │  subscribes harvest/show/# ; publishes harvest/show/leader/status (heartbeat)
   │
   │  on a cue: originate the ESP-NOW broadcast burst (§3.4 H1/H2), THEN play its
   │  own audio, THEN fetch MIDDLE (§2.2)
   ▼  ESP-NOW broadcast (AA:BB:CC:DD:EE:FF, 22-byte Msg, ch 11)
   ├─────────────► every prop: zombie container, singing pumpkins, spooky tree,
   │               stain glass, parkway 1/2, area lights, crows, …
   │               → latch (theme,tone) by session_id, run own logic independently
   │               (Tier 1 — see docs/fleet-communication-tiers.md)
   └─────────────► Skeleton A1S (0x07): latches theme/tone off the broadcast,
        ▲          waits for CMD_NARRATOR_2_START, then plays (huge budget — H2)
        │
        └── unicast, ESP-NOW native confirmation, ≤3 tries, non-blocking (§3.4
            H1b): Scarecrow→Burt cue receipt, then Burt→Scarecrow "starting" ack.
            The ONE Tier-2 pair among the narrators.

Main Switch Controller (0x01) — FALLBACK ISLAND, not in the normal path.
   Local PIR + manual tone button. Armed only by a dead-man (§3.4 H4).

Eave Coordinator (0x0D) — UNCHANGED. Calendar + lighting only. Disjoint traffic.
```

### 3.2 Cue flow, end to end

**Pre-show (sidewalk trigger):**
1. Python locks `(session_id, theme, tone)` from `theme_deck.py`. Publishes
   `harvest/show/params` (retained) then `harvest/show/cue` phase `preshow`.
2. Scarecrow A1S receives the cue. **In order (§3.4 H2):**
   a. Broadcast the ESP-NOW burst — `CMD_SET_THEME`, `CMD_SET_MODE`,
      `CMD_SHOW_STATE(SHOW_NARRATOR)` — the 3-frame group sent **3×**, ~150–300 ms
      apart (§3.4 H1).
   b. Start its own INTRO from SD (§2.1).
   c. Begin the MIDDLE fetch (§2.2), crossfade MIDDLE in when the INTRO ends.
3. Scarecrow finishes its INTRO+MIDDLE → broadcasts `CMD_NARRATOR_1_DONE` and
   `CMD_NARRATOR_2_START` (`payload_a=theme, payload_b=tone`), **and** unicasts
   `CMD_NARRATOR_2_START` to `MAC_SKELETON_A1S` with native confirmation (§3.4
   H1b — ≤3 tries, then proceed regardless).
4. Skeleton A1S plays its pre-show line. On start it unicasts a "starting" ack
   back to the Scarecrow (§3.4 H1b, diagnostic only), then broadcasts
   `CMD_NARRATOR_2_DONE` when finished.
5. Theme loop runs on every prop. Everyone has `(theme,tone)` from step 2a.

**Hold phase (`phase: "hold"` — the taunt loop, added 2026-09-08):**
Python fires this every `HOLD_INTERVAL_S` (~40 s) that the visitor stays in
`waiting_zone` without reaching `brick_walkway`, capped at 3
(`docs/show-trigger-geometry.md` §2c). Same `session_id`, **same locked
`(theme, tone)`** — every retrigger reuses the pair locked at the waiting-zone
trigger, no re-roll. Scarecrow plays
`/scarecrow/hold/<theme>_<tone>/%03d.wav` → `CMD_NARRATOR_2_START` → Burt's
`hold/` line → `CMD_NARRATOR_2_DONE`. **No** ESP-NOW theme burst, **no**
`SHOW_STATE` change — the theme loop keeps running underneath. Anti-repeat on the
`hold/` index matters most here (a visitor must not hear the same taunt twice in
40 s). If the `hold/` pool isn't on the card yet, fall back to `bridge/generic`.

**Main show (brick-walkway trigger):**
6. Python publishes `harvest/show/cue` phase `mainshow` (same `session_id`).
7. Scarecrow A1S: broadcast `CMD_START_SCENE` (session_id, `payload_a=theme`) —
   again 3× — then play its OUTRO from SD.
8. `CMD_NARRATOR_1_DONE` → `CMD_NARRATOR_2_START` (broadcast + H1b unicast to
   Burt) → Skeleton OUTRO (+ H1b "starting" ack back) → `CMD_NARRATOR_2_DONE`.
9. **Linear sequence** (`docs/fleet-communication-tiers.md` "The show sequence"):
   Scarecrow → Burt → then **no more narration**. On `CMD_NARRATOR_2_DONE` the
   Scarecrow broadcasts `CMD_SHOW_STATE(SHOW_ACTIVE)` + starts the fixed
   `SHOW_ACTIVE_MS` timer, which fires the **creepy-theme-music phase** — Spooky
   Tree + spatial L/R (theme ambient/music) **and** Singing Pumpkins, all
   starting together, plus `theme_props()`. Runs off that one broadcast state —
   no per-prop presence logic.
10. When `SHOW_ACTIVE_MS` expires: Scarecrow broadcasts
    `CMD_SHOW_STATE(SHOW_OFF)` → (2 s) → `SHOW_IDLE`, Python clears
    `harvest/show/params`, everything resets together, Scarecrow re-arms for
    Frigate.
    **`SHOW_ACTIVE_MS` = `210000` — CONFIRMED 2026-09-06** (one standard show =
    3 min 30 s; three theme shows). Now a named shared constant in
    `ProjectConstants.h`. Theme-music content is authored to fit inside it, not
    the reverse. Still to fix in code: `main_switch_controller/src/main.cpp:218`
    (`180000` literal) and `showsim/constants.py` (add the constant).
    END/reset-signal owner deliberately open.
    (The Zombie Container DMX segment fires at the ROTLD narration start when the
    touchpad panel is armed — `docs/zombie-container.md` — and runs its own clock
    in parallel; not part of this timed sequence.)

The vestigial bits this retires: `CMD_NARRATOR_ACK` ("A1S → main: switching to
WiFi") is meaningless now — the A1S is always on WiFi and is itself the
originator. The 15 s ACK / 5-min dead-man from `inventory-report.md` §5 is
replaced by the §3.4 H4 dead-man.

### 3.3 Coexistence — Main Switch (fallback) and Eave (untouched)

**Main Switch Controller — demoted to a fallback island.**
- Normal operation: it does **nothing** to originate the show. It may still
  *listen* (latch theme for any local effect it drives) but it does not roll a
  deck, does not broadcast theme/tone, does not relay Linux traffic.
- Its `nextTheme()` deck stays in the firmware but is **vestigial** — it only
  executes in standalone fallback mode.
- Fallback trigger: see §3.4 H4. When armed, Main Switch runs a self-contained
  show — its own PIR for the trigger, `nextTheme()` for the theme, its manual
  button for tone, its own `CMD_SET_THEME`/`CMD_SET_MODE`/`CMD_START_SCENE`
  broadcasts. Degraded (no vision MIDDLE, no per-visitor tone) but the yard still
  runs.
- **Manual tone override in normal operation:** the Main Switch tone button still
  needs to reach the authority. Main Switch broadcasts `CMD_FORCE_MODE`
  (`payload_a=ShowMode`) over ESP-NOW on any button change. The Scarecrow A1S
  hears it, applies it locally (manual always wins — Spec v2 §2), **and**
  republishes it to `harvest/show/manual_tone` so Python's controller state
  matches. No new command needed — `CMD_FORCE_MODE = 0x25` already exists.

**Eave Coordinator — no change, no conflict.**
- Eave keeps doing exactly what `inventory-report.md` §6 describes: 6-byte
  `CmdPacket` to Parkway 1/2 + Area lights, calendar-driven state, replies to
  `CMD_CALENDAR_REQUEST`.
- Why there's no collision with the A1S now also being a WiFi→ESP-NOW bridge:
  - **Disjoint command sets.** Eave emits `CmdPacket` (6-byte) and calendar
    replies; the A1S emits show `Msg` (22-byte) `SET_THEME`/`SET_MODE`/
    `START_SCENE`/`NARRATOR_*`/`SHOW_STATE`. A node tells them apart by frame
    size and `command`. Neither interprets the other's frames.
  - **Same medium, no arbitration needed.** Both are fire-and-forget broadcasts
    on the ch-11 single-peer `FF:…:FF`. ESP-NOW/WiFi CSMA handles the air;
    there's no shared state to corrupt. This is already true today (Eave and Main
    Switch both broadcast).
  - **Different lifetimes.** Eave runs year-round; the A1S leader only exists
    during show season. Outside the season the fleet takes lighting cues from
    Eave and there simply is no show authority — which is correct.
  - The A1S leader must **not** send `CmdPacket`s or answer `CMD_CALENDAR_REQUEST`
    — that stays Eave's. Enforced by simply not implementing it in A1S firmware.

### 3.4 Hardening — decisions, locked

**H1 — Redundant broadcast, NO custom ACK/retry (fleet-wide).**
Every fleet-facing cue (`SET_THEME`+`SET_MODE`+`SHOW_STATE` group;
`START_SCENE`; `NARRATOR_*`) is **broadcast** (single `FF:…:FF` peer) and
transmitted **3 times, ~150–300 ms apart**, as cheap insurance against a single
dropped frame. This matches the existing broadcast-only mesh contract ("the show
goes on if a node misses a cue").

- **No custom acknowledgement packet. No retry handshake. No pairing ceremony.**
  Broadcast 802.11 frames are un-acked at the link layer by design; building a
  custom ACK/retry protocol on top would re-introduce exactly the failure mode
  (a stalled sender waiting on a silent peer) this architecture exists to avoid.
- Receivers **dedupe on `session_id`** (+ `command`): the 2nd and 3rd copies are
  no-ops. The `spooky-tree` handler's 8-slot ring buffer and whole-queue drain
  already tolerate a 3× burst (`espnow_handler.cpp` / IMPROVEMENTS.md 1.1).
- `seq` still increments per frame so a sniffer can see all copies; latching is
  keyed by `session_id`, not `seq`.
- The one place a delivery *check* is wanted — the tight Scarecrow→Burt narrator
  seam — gets it from ESP-NOW's **native** unicast confirmation, not a custom
  packet. See **H1b**.

**H1b — Scarecrow↔Burt soft handshake (ESP-NOW native unicast confirmation).**
The two narrator A1S nodes are genuinely codependent — Burt speaks straight after
the Scarecrow in a tight sequence — so the Scarecrow↔Burt pair (and *only* that
pair) adds a lightweight delivery check **on top of** the unchanged fleet
broadcast. It uses a feature the protocol already provides, not new protocol
logic:

- **Mechanism.** ESP-NOW *unicast* sends produce a real link-layer delivery
  result in the send callback (`esp_now_send_cb_t` → `ESP_NOW_SEND_SUCCESS` /
  `ESP_NOW_SEND_FAIL`), because unicast frames get an 802.11 ACK. No custom ACK
  frame, no reply packet needed for the "did it arrive" question.
- **Peer cost.** Each narrator adds **one** unicast peer entry for the other's
  MAC (`MAC_SKELETON_A1S` / `MAC_SCARECROW_A1S`) alongside the shared broadcast
  peer. One slot each — nowhere near the 20-peer cap (the fleet is still reached
  through the single `FF:…:FF` entry).
- **Cue direction (Scarecrow → Burt), at the pre-show / main-show cue:**
  1. Fleet broadcast burst as always (H1) — unchanged, reaches Burt too.
  2. *Additionally* unicast the **same** `CMD_NARRATOR_2_START` frame
     (`payload_a=theme, payload_b=tone`, same `session_id`) directly to
     `MAC_SKELETON_A1S`.
  3. Send callback says `SUCCESS` → done, proceed.
  4. Callback says `FAIL`, or no callback within ~50 ms → retry. **Up to 3
     attempts total, ~20–50 ms apart** (closely spaced, not backed off).
  5. After 3 attempts, **proceed with the show regardless of outcome.** If Burt
     truly never heard it, his own fallback ladder (§2.3 bridge line, etc.)
     covers him. Never block.
- **Handoff direction (Burt → Scarecrow), symmetric:** when the Scarecrow's
  monologue finishes and Burt starts, Burt unicasts a `CMD_NARRATOR_2_START`-ack
  / "starting" frame (`CMD_ACK 0x43`, `payload_a=NODE_SKELETON_A1S`,
  `payload_b=session_id`) back to `MAC_SCARECROW_A1S`, same pattern — native
  confirmation, ≤3 closely-spaced attempts, never blocking. This is
  **informational / diagnostic only** — the show does not wait on it to
  advance.
- **Logging (both directions).** Record `success|fail`, attempt count, and the
  final proceed-anyway decision, per cue, to the event log (and, on the
  Scarecrow, publish a rollup to `harvest/show/leader/status`). This is the
  cheap early-warning that Burt's node or the RF path between the two figures is
  going flaky before it becomes a visible show failure.
- **Not built:** any custom ACK-packet protocol, any MAC-pairing ceremony, any
  blocking wait loop. Three bounded tries on the native result, then move on.
- **VERIFIED 2026-09-06 on both real boards.** Firmware: Scarecrow
  `fleet.cpp` (`fleet_cue_burt()` + the `CMD_ACK` RX handler), Burt
  `burt_link.cpp` (`burt_send_starting_ack()` + the `CMD_NARRATOR_2_START` RX
  handler, deduped on `(session_id, phase)`). Live: 3/3 cycles each direction,
  native `ESP_NOW_SEND_SUCCESS` on the 1st attempt both ways, Scarecrow received
  every Burt ack, neither side blocked. Observed: Burt fires his starting-ack off
  the **broadcast** copy of the cue (mid-burst, before the unicast lands) and the
  unicast then confirms — exactly the intended belt-and-suspenders behaviour.

**H2 — Sequencing, not tight timing.**
The time budget is generous — the Skeleton's cue isn't needed until the
Scarecrow's full monologue **plus** the visitor's walk down the path (seconds of
slack, not milliseconds). So the firmware **deliberately orders** radio actions
instead of assuming they're free to overlap:
1. ESP-NOW cue burst first (short — a few frames, tens of ms).
2. *Then* start local SD playback (no radio).
3. *Then* start the sustained HTTP MIDDLE fetch.
Rationale: get the one cue the fleet depends on off the air **before** a
multi-second download competes for the radio. Bench testing may show a better
order (e.g. kick off the fetch's TCP connect, *then* burst, *then* stream the
body) — the firmware exposes the order as a small tunable and the §1.6
coexistence test picks it. What is **not** allowed: firing the burst and starting
the download "at the same instant" and hoping.

**H3 — WiFi STA + ESP-NOW on one radio, channel 11.**
The A1S holds a WiFi STA association (for MQTT + audio HTTP) and transmits
ESP-NOW. ESP-NOW rides the STA's channel; every property AP is locked to
**channel 11** (`reference_property_network_channel11.md`,
`NETWORK_CONSTRAINTS.md` C1) precisely so this works. Firmware rules:
- Associate only to a ch-11 AP; on `WiFi.channel() != 11`, log the drift and
  re-associate — never transmit ESP-NOW off-channel (the standing per-node
  channel-drift check).
- `esp_wifi_set_ps(WIFI_PS_NONE)` + `WiFi.setSleep(false)` (same as every mesh
  node — needed so ESP-NOW RX isn't missed, at a current cost §1.5 covers).
- Broadcast from the main loop, never from the MQTT or ESP-NOW RX callback.
- **VERIFIED on the actual board 2026-09-06 — PASS.** `fleet_coex_test()`:
  associated on ch 11, 60/60 ESP-NOW broadcasts landed while a 200 KB HTTP GET
  completed in full, association held, channel held, RSSI stable — and again
  with I2S playback active. **The mesh-gateway fallback is not needed.** The
  standing channel-drift check (`WiFi.channel() != 11 → log + re-associate`) is
  implemented in `fleet.cpp`.

**H4 — Failure independence (A1S down ⇒ fleet still runs).**

> **UPDATED 2026-09-24 (CURRENT-BUILD #12):** the Main Switch (and parkway) are
> retired, so the dead-man takeover below no longer exists. The Scarecrow's
> 10 s heartbeat is now **`CMD_HEARTBEAT`** (liveness only, carries no show
> state; nothing acts on it). It used to be `CMD_SHOW_STATE(SHOW_IDLE)`, which
> told every prop the show had ended ~10 s into the SHOW_ACTIVE window. Real
> state changes are still their own 3x `CMD_SHOW_STATE` bursts: `SHOW_NARRATOR`
> at PRE-SHOW, `SHOW_ACTIVE` after the narrators, `SHOW_OFF`/`SHOW_IDLE` when
> the 210 s window expires. A bridge `reset` cue during an open window is
> deferred (it used to cut the window ~2 s after the narrators). The text below
> is the original design, kept for history.

- Every prop already runs its own logic off broadcasts and degrades gracefully
  if a cue is missed — unchanged.
- The Scarecrow A1S broadcasts `CMD_SHOW_STATE` as a **leader heartbeat every
  ~10 s** whenever it's healthy (idle or mid-show).
- **Main Switch arms its standalone fallback on a dead-man:** no
  `CMD_SHOW_STATE` from `sender == 0x06` for **90 s** ⇒ Main Switch assumes the
  leader is gone and takes over (local PIR + `nextTheme()` + manual tone). When
  the A1S heartbeat returns, Main Switch stands back down at the next
  `SHOW_IDLE`. The 90 s window makes a double-broadcast (both the A1S and Main
  Switch driving a show) essentially impossible.
- Python side: if it can't publish to the broker, or sees no
  `harvest/show/leader/status` for 90 s, it logs the outage and stops trying to
  drive cues — the Main Switch dead-man is the recovery path, and Python
  re-syncs (retained `params`) when the leader is back.
- The A1S does **not** need to recover before the fleet can fall back — the
  dead-man is entirely Main-Switch-local.

### 3.5 Message format — reuse the existing 22-byte `Msg`, no new wire type

Everything needed is already in `shared/Shared_Constants/ProjectConstants.h`:

| Field | Value on the leader's broadcasts |
|---|---|
| `sender` | `NODE_SCARECROW_A1S` (`0x06`) — the actual transmitter; it's already an authority-role node (`docs/scarecrow.md`) |
| `target` | `0xFF` broadcast |
| `session_id` | the `uint32` Python minted at sidewalk-lock (§3.6) |
| `CMD_SET_THEME 0x02` | `payload_a` = `ShowTheme` (CORN 0 / ROTLD 1 / MUTANT_PUMPKINS 2) |
| `CMD_SET_MODE 0x03` | `payload_a` = `ShowMode` (FRIENDLY 0 / SCARY 1 / INTENSE 2 — inert, → Scary) |
| `CMD_SHOW_STATE 0x28` | `payload_a` = `ShowState` (real state changes only; since 2026-09-24 NOT the heartbeat) |
| `CMD_HEARTBEAT 0x40` | Scarecrow leader liveness every ~10 s, no payload, no state (2026-09-24) |
| `CMD_START_SCENE 0x01` | main-show fire; `payload_a` = `ShowTheme` |
| `CMD_NARRATOR_2_START 0x2D` | `payload_a` = theme, `payload_b` = tone (belt-and-suspenders for the Skeleton) |

The `spooky-tree` handler comment already anticipates "a full `SHOW_LOCK` +
`SET_THEME` + `SET_MODE` burst" — **the fleet is already built for this.** No new
`Command` value, no protocol bump.

### 3.6 session_id ownership, timing, reboot resync

- **`session_id` authority is the Python orchestration.** It mints a `uint32` at
  sidewalk-lock and reuses it across the MQTT messages, the A1S's ESP-NOW bursts,
  the `CMD_NARRATOR_*` payloads, the `middle/<session_id>_*.wav` URL, and the
  event log. (Firmware `esp_random()` session IDs remain only for Main Switch's
  standalone fallback.)
- **Timing has margin.** Theme locks at the sidewalk trigger; main-show props
  don't fire until the brick-walkway trigger, tens of seconds later. MQTT→A1S is
  <100 ms on the LAN; the 3× burst spans <1 s. Everyone has the theme long before
  it's needed. (The tight path in this project is the *Scarecrow INTRO tone*
  selection — a separate problem owned by `docs/_history/dialogue-architecture.md`.)
- **Reboot resync.** `harvest/show/params` is **retained**, so the A1S re-reads
  the current cycle on reconnect. A prop that reboots mid-cycle broadcasts
  `CMD_NODE_ONLINE`; the A1S answers with a fresh theme burst. At cycle end
  Python clears `params` so a late reboot can't latch a stale theme.
- **No mid-cycle re-roll** (Spec v2) — a prop already mid-show ignores a new
  theme until it's back at `SHOW_IDLE`.

---

## Decisions — status

**Locked 2026-09-06 (this revision):**
- A1S (Scarecrow) is the fleet leader + WiFi→ESP-NOW bridge. Main Switch →
  fallback island. Eave untouched. (§3.1, §3.3)
- Theme-deck authority is Python `theme_deck.py`; firmware `nextTheme()` is
  vestigial / standalone-fallback only. (§3.1)
- Redundant 3× broadcast, no custom ACK/retry. (§3.4 H1)
- Scarecrow↔Burt soft handshake = ESP-NOW **native** unicast confirmation, ≤3
  closely-spaced tries each way, never blocking; both directions logged. It is
  the only narrator-tier unicast relationship. (§3.4 H1b;
  `docs/fleet-communication-tiers.md`)
- Deliberate radio sequencing: cue burst → SD playback → HTTP fetch. (§3.4 H2)
- MQTT (`harvest/show/params` retained + `harvest/show/cue`) is the Linux↔A1S
  transport; the old UDP `:4210/:4211` relay via Main Switch is retired. (§3.1)
- Main Switch dead-man = 90 s with no leader `CMD_SHOW_STATE`. (§3.4 H4)
- MIDDLE = push-then-play, not streaming. Fallback ladder per §2.3. (§2.2)
- Board connectors are micro-USB. (Part 0, §1.5)

**Bench Step 1 — DONE 2026-09-06:**
1. ~~**Codec**~~ → **ES8388** (I²C 0x10). Confirmed.
2. ~~**DIP switches**~~ → shipped **1,2,3 ON / 4,5 OFF** = clean 1-bit SD bus +
   DAT3 pull-up, GPIO12 strap unloaded. **Keep as-is, no switch change.** Firmware
   uses `SD_MMC.setPins(14,15,2)` + 1-bit `begin()`.
3. ~~PSRAM~~ → **4.00 MB, usable.** Chip = ESP32-D0WD-V3 rev 3.1, 4 MB flash.
4. This board's MAC = `MAC_SCARECROW_A1S` → it's the **leader** node.

6. ~~**WiFi+ESP-NOW coexistence on this A1S, ch 11, under load**~~ → **PASS
   2026-09-06** (§3.4 H3). Leader role confirmed viable; mesh-gateway fallback
   dropped.

8. ~~**Live Scarecrow↔Burt handshake**~~ → **VERIFIED 2026-09-06 on both real
   boards** — SUCCESS path both directions, 1st-attempt native confirmation,
   3/3 cycles, non-blocking (§3.4 H1b). Burt firmware:
   `props/skeleton-narrator-a1s/` (env `skeleton`).
9. **MACs** → both **bench-confirmed** off the chips (Scarecrow
   `AA:BB:CC:DD:EE:FF`, Burt `AA:BB:CC:DD:EE:FF`); `ProjectConstants.h` matched
   exactly, no change needed.

**Production audio — DONE 2026-09-07:** full render complete + OP-2 verified
(1,260 files, 1,200 INTRO/OUTRO + 60 bridge). SD staging trees built at
`orchestration/voice/sd-card/{scarecrow,burt}/`. **On-card file format is
settled — `docs/sd-card-format.md`:** the A1S reads files **by explicit path,
not FAT/copy order**, so files just need to be `000.wav…(count-1).wav` under
`/scarecrow/` (or `/burt/`) with `index.csv`. ~2–3 length-outlier lines flagged
for Joe's spot-check (`orchestration/voice/out/pools/LENGTH_OUTLIERS.txt`), not
a blocker.

**Still needs real hardware (not blocking further firmware work):**
5. **SD card mount** — needs a physical microSD card loaded from the staging
   tree. DIP is already right; expect a clean 1-bit mount. `pools.cpp` runs the
   placeholder path until a card is in. Applies to both nodes.
7. **Audible audio** — needs a powered speaker + PicoTalk on LINE OUT (both nodes).
10. **Main-show OUTRO cue** (`CMD_START_SCENE` → Burt `mainshow=1`) — Burt handles
    it; the Scarecrow cycle-sim only runs the pre-show flow, so not exercised live.

**Joe's call (preference, not blocking):**
4. **Storage sample rate** — 24 kHz vs 16 kHz mono WAV for the on-card pools.
   Staging defaults to **24 kHz** (still ~0.5 GB/card); `--sr 16000` halves it.
   Firmware plays either — `wav_play()` reconfigures the codec per file.
5. **Bridge pool** — **DONE**: 30 Scary theme-agnostic lines/character rendered.
   Joe spot-checks the audio.

---

## References

- `docs/a1s-bringup-test.md` — the throwaway audio-path bring-up sketch (Linux
  half done, ESP32 half blocked on firmware)
- `docs/_history/dialogue-architecture.md` — INTRO/MIDDLE/OUTRO seam design, live MIDDLE
  pipeline, tone-timing, "the show goes on" guarantee
- `docs/fleet-communication-tiers.md` — per-prop Tier 1 (broadcast-only) vs
  Tier 2 (unicast native confirmation) classification; the Scarecrow↔Burt
  pattern (§3.4 H1b) applied fleet-wide
- `docs/scarecrow.md`, `docs/skeleton.md` — the two narrator props (voice locked,
  PicoTalk hardware, no firmware yet)
- `docs/inventory-report.md` §5–6 — show trigger sequence + narrator cue chain
  (its "Linux Master via Main Switch UDP relay" and "15 s ACK / 5-min dead-man"
  are **superseded** by this doc: MQTT→A1S transport, §3.4 H4 dead-man) and §6
  Eave Coordinator role (unchanged)
- `shared/Shared_Constants/ProjectConstants.h` — 22-byte `Msg`, `Command`,
  `ShowTheme`, `ShowMode`, `NodeID`, MACs, CRC16
- `props/spooky-tree/src/espnow_handler.cpp` — the canonical broadcast-only
  ESP-NOW node pattern (ring buffer, IDF4/5 bridge, channel lock, `SET_THEME` +
  `SET_MODE` burst expectation)
- `orchestration/showsim/` — Show Logic Spec v2 PoC (theme deck, tone precedence,
  cycle state machine)
- `orchestration/voice/README.md`, `orchestration/voice/audio_bridge.py` — the
  render + HTTP-serve toolchain (`:8080`, Workshop `192.168.1.x`)
- Memory: `reference_property_network_channel11.md` / `NETWORK_CONSTRAINTS.md` C1
  — all APs and mesh nodes locked to channel 11

### External sources consulted

- [DIP switches on Ai-Thinker ESP32 Audio Kit V2.2 — arduino-audio-tools #68](https://github.com/pschatzmann/arduino-audio-tools/issues/68)
- [trombik/esp-adf-component-ai-thinker-esp32-a1s — board definition](https://github.com/trombik/esp-adf-component-ai-thinker-esp32-a1s)
- [ESP32-AUDIO-KIT — NuttX board port (pin table)](https://nuttx.apache.org/docs/latest/platforms/xtensa/esp32/boards/esp32-audio-kit/index.html)
- [chipmuenk/modular_synths — esp32_a1s.md pin map](https://github.com/chipmuenk/modular_synths/blob/main/esp32_a1s.md)
- [Phil Schatzmann — "The AI Thinker Audio Kit Experience (or nothing is right)"](https://www.pschatzmann.ch/home/2021/12/06/the-ai-thinker-audio-kit-experience-or-nothing-is-right/)
- [Phil Schatzmann — Using the AI Thinker Audio Kit Buttons](https://www.pschatzmann.ch/home/2021/12/08/using-the-ai-thinker-audio-kit-buttons/)
- [ES8388 vs AC101 — marcel-licence/esp32_midi_sampler discussion #50](https://github.com/marcel-licence/esp32_midi_sampler/discussions/50)
- [Ai-Thinker-Open/ESP32-A1S-AudioKit — vendor SDK + schematic](https://github.com/Ai-Thinker-Open/ESP32-A1S-AudioKit)
- [homeding — Ai-Thinker ESP32 Audio Kit board notes](https://homeding.github.io/boards/esp32/esp32audiokit.htm)
