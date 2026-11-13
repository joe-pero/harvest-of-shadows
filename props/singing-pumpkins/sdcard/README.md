# Singing Pumpkins — BrightSign SD card

The show logic lives **on the XT1143's microSD card** as a plain BrightScript
`autorun.brs`. No BrightAuthor presentation. The **ESP32 (ESP-NOW node) is the
controller**: it hears the Scarecrow's show state on the mesh and sends one-word
serial lines; the card decides what to play.

## Files

| file | what |
|---|---|
| `autorun.brs` | the show logic; the player runs it at power-up |
| `show.json` | knobs you edit: DJ shares, run length, show length, volume, END behaviour |
| `library.json` | *generated*: every video with its number `n`, face set, category, duration |
| `TRACKS.md` | *generated*: duration table for every clip (also written to the card) |
| `media/<family>/…mp4` | *generated*: staged videos (safe file names, normalized audio) |
| `media/transitions/*.mp4` | *generated*: 6 iris transitions, one per face-group change (`atmos_to_tsp.mp4` …) |
| `build_card.py` | builds the card folder from `/mnt/media/Pumpkin folder 2026/Main 2026/` |
| `sim_card.py` | offline simulator of `autorun.brs` against the real library, used to tune `show.json` |

```
python3 build_card.py --dry-run     # see how every file is classified
python3 build_card.py               # stage -> /mnt/media/Pumpkin folder 2026/SD Card 2026/
python3 sim_card.py                 # 8 h DJ + 200 show plans, prints shares/lengths
```
Then copy the **contents** of `SD Card 2026/` to the root of the card
(`autorun.brs` must be at the root). Staging is incremental; re-run after adding videos.
**Adding or removing a video renumbers the jukebox `n` values**; check `TRACKS.md`.

## Behaviour

**DJ mode** (default, and after every show): shuffle by face set with a target
share of *playing time* (`dj_share`, default Suno 40 / TSP 30 / Atmos 30; Atmos
= songs 60 / tales 35 / scares 5). Up to `max_run` (2) in a row from one face
set, no song again until `no_repeat_window` (25) others have played. Same face
set = back to back, no fade. **Face set changes = one iris transition file**
(2.5 s, silent): the old group's resting faces close into the centre of each
pumpkin, the new group's resting faces open out of the same centres (Joe's
"center fade"). Centres/radii measured from all the face sets:
(344,622) r279 · (1010,586) r263 · (1628,620) r196, opened to 1.6× so TSP's
taller faces aren't clipped. Resting footage: Atmos = flickering candle buffer,
TSP = start of Joe's `out fade`, Suno = first seconds of "Blood in the Corn".
Preview: `/mnt/media/Pumpkin folder 2026/Transition previews/All 6 transitions preview.mp4`
(song end → transition → next song start, all 6). Joe's own buffer/fade clips
are staged as category `buffer` but not used by the logic.

**SHOW mode** (`SHOW` from the ESP32): cut the current song → transition to Atmos (if not already Atmos) →
one **Opening** → shuffled **Clips 2.0** that fit → one **Close out**, planned
from the known durations to fit `show_seconds` (150; Joe: 2 to 2.5 min after Burt. 2000 simulated plans = 134-150 s). Then back to DJ. A second
`SHOW` during a show or within `show_cooldown_seconds` is answered `BUSY`.
`END` drops the remaining middle clips and plays the close-out
(`end_mode`: `after_clip` or `now`).

Simulated against the staged card (sim_card.py, 8 h): Suno 40.5% / TSP 31.2% /
Atmos 28.2% of play time, ~11 face changes per hour, bridges 3.2% of airtime;
show plans 165–180 s with 2–7 middle clips.

Staged card verified 2026-09-27: 164 files (162 + 2 generated Suno bridges),
5.4 GB, none missing from library.json; spot-checked loudness −14.5 to −16.7 LUFS
(Dead Man's Party −1.6 → −15.1). Tempo scan (librosa, `tempo_scan_2026-09-27.jsonl`):
median BPM TSP 123, Atmos 123, Suno 96. Not used by the shuffle yet.

**Safety nets:** each clip has a watchdog (duration + 8 s) that skips a stuck
file; a file that fails to open is reported and skipped (10 in a row → retry
in 5 s); a stale end-of-media event within 1 s of a new start is ignored;
missing `show.json` falls back to built-in defaults; every problem goes out
the serial port as an `ERR …` line.

## Serial protocol (115200 8N1, lines end in CR)

| ESP32 → card | effect |
|---|---|
| `SHOW` | start the trick-or-treat show (`BUSY` if one is running or in cooldown) |
| `SHOW FORCE` | manual override: fresh show now, ignores running show + cooldown |
| `END` | finish the show early (close-out) |
| `PLAY <n>` | jukebox: `n` next |
| `PLAY <n> NOW` | manual override: `n` right now; ends a running show (no close-out) |
| `LIST` | pick list: `SONG <n> <label>` per song/tale/scare/ToT clip, then `LISTEND <count>` |
| `SKIP` · `VOL <0-100>` · `STATUS` · `PING` | helpers |

Card → ESP32: `READY items=…`, `NOW <n> <id> <family> <category> <dur>`,
`MODE SHOW clips=… planned=…s` / `MODE DJ`, `ACK …`, `BUSY …`, `ERR …`,
`HB mode=… now=…` every 30 s (the ESP32's link check), `STATUS …`, `PONG`.

## Home Assistant (manual override / jukebox)

Joe 2026-09-27: HA is the main override (the touchpad path stays, secondary). The
pumpkin ESP32 joins WiFi (ch-11 AP, same channel as ESP-NOW) and talks MQTT to
**HA's broker on detect-server (192.168.1.x)**, not the Workshop show broker.
HA auto-discovery creates a **"Singing Pumpkins"** device, no YAML needed:

| entity | does |
|---|---|
| select **Play now** | every song/tale/scare/ToT clip on the card (153, e.g. `12 - Suno - Cornfield After Dark (Barbershop)`) → `PLAY n NOW` |
| button **Start show** | `SHOW FORCE` (run it early, re-run after a misfire) |
| button **End show** | `END` (close-out, then back to the loop) |
| button **Skip** · number **Volume** | `SKIP` · `VOL n` |
| sensors **Now playing** · **Mode** · **Last card error** · binary **Player link** | what the card reports; link = heartbeat in the last 75 s |

The pick list comes from the card itself (`LIST` at boot / on card `READY`), so
HA always matches what's on the SD card: rebuild the card, reboot the player,
and the menu updates. Raw backdoor: publish any card line to `harvest/pumpkins/cmd`.
Checked offline: the select payload is valid JSON, 7.1 KB (ESP32 MQTT buffer 12 KB,
serial RX buffer 10 KB for the 7.8 KB list). **Not yet seen in HA.**

## Wiring (garage bench first, then the front yard)

The HW-027 is the mini blue MAX3232 board (silkscreen `RS232<->TTL`, "110"). Each
side has four pins: two arrows, a `+` and a `−` (printed as a short bar). With the
text readable, the **left column is RS-232** (to the player) and the **right column is
TTL** (to the ESP32).

```
 BrightSign XT1143            HW-027 left (RS-232)  |  HW-027 right (TTL)          ESP32 (esp32dev)
 3.5 mm TRS cable                                   |                              22/4 alarm wire
   Tip    red   (player RX) <--- ← (TX, ~−5.5 V)    |   −  ------------------ black --- GND
   Ring   white (player TX) ---> → (RX, ~0 V)       |   +  ------------------ red ----- 3V3 (NOT 5V)
   (nothing)                     +  LEAVE EMPTY     |   ← (into board, RXD) <- green --- GPIO22 (TX)
   Sleeve black (GND)       ---- −                  |   → (out of board, TXD) -> white - GPIO21 (RX)
```

- **Power only one side.** The board has a `+` on both sides and the seller warns that
  only one power input may be connected. Power the right (TTL) `+` from the ESP32;
  the left (RS-232) `+` stays empty. The two `−` pins are the same ground (check they
  beep to each other), so the plug sleeve goes on the left `−`.
- The 3.5 mm cable is a 3-pole (TRS) repair lead: red = left = tip, white = right =
  ring, black = sleeve. Confirm with the meter anyway. The ESP32 run is 22/4 solid
  alarm wire (red 3V3, black GND, green GPIO22, white GPIO21). Both runs use red,
  white and black for different things, so wire by position, not colour. A spare
  harness is being made to the same pattern.
- **Power the HW-027 from the ESP32's 3.3 V pin, never 5 V.** The MAX3232 drives its
  TTL output at its own supply voltage; at 5 V it would push 5 V into GPIO21 (the
  ESP32 is not 5 V tolerant).
- **Cable colours are not standard.** Identify each wire of the 3.5 mm cable with a
  meter in continuity mode against the plug's tip / ring / sleeve before soldering.
  Thin headphone wires are often enamel-coated: scrape or tin them first.
  A 4-conductor (TRRS) plug in the XT1143's 3-conductor jack: tip = tip, first ring
  = ring, sleeve = GND; the second ring is unused (insulate it).
- **Confirm the RS-232 TX pin with a meter** (HW-027 powered, black probe on GND):
  its RS-232 **TX pin idles about −5 to −6 V**; the RX pin sits near 0 V. That TX pin
  goes to the plug **tip**. Same test on the player side with the player on: the
  **ring** (player TX) idles at a steady non-zero voltage. If the HW-027 has a DB9, its
  TX is pin 2 or 3 depending on the board: the meter decides, not the pin number.
- **TTL arrows:** the arrow pointing out of the board is data leaving the HW-027 →
  ESP32 RX (GPIO21); the arrow pointing into the board is data arriving → from ESP32
  TX (GPIO22). If `PING` gets no `PONG`, swap these two wires first: swapping TX/RX
  on either side cannot damage anything.
- Pins are defined in `src/brightsign.h` (`BS_TX_PIN` 22, `BS_RX_PIN` 21).

**Bench results 2026-09-27 (link WORKING):**
- BrightSign's own XTx43 docs confirm the jack: **Tip = player RX, Ring = player TX, Sleeve = GND** (the map
  above). This older player (OS 6.2.63.3, pre-rev-H) does **not** idle its TX at RS-232 levels: the ring read
  about +1.3 V with nothing attached and ~0 V (auto meter beeps) on the HW-027. The MAX3232 still reads it fine.
- Script serial port = **0**, the only one the player has (`boot_ok.txt` lists ports 0-3).
- **The first HW-027 had a dead receive side** (ESP32 got 0 bytes, even with tip+ring bridged and both reading
  -6.1 V). Marked bad. The second harness worked first time. Board check without the player: bridge tip and
  ring on the plug, send `PING` from the ESP32 console; a good board echoes it back.
- ESP32 console extras (not sent to the card): `SWAP` flips TX/RX live until reboot, `PINS` shows the pins
  and the byte count received from the card (0 = nothing arriving on the RX wire at all).
- Your meter is auto-ranging: ohms instead of volts means *no voltage there*.

### Before every card build: BrightScript check

`sim_card.py` tests the show logic in Python, not the BrightScript itself. Run the real syntax check too:

```
mkdir -p /tmp/bsc/source && cp autorun.brs /tmp/bsc/source/ && printf 'title=t\n' > /tmp/bsc/manifest
cd /tmp/bsc && npx -y -p brighterscript bsc --rootDir . --createPackage false
```

It is a Roku checker: `BS1129 Unknown BrightScript component` for `roSerialPort`, `roVideoMode`, `roTimer`,
`roSystemTime` are false alarms. **Anything else is real.** BrightScript is case-insensitive: a variable with
a function's name (`txt` vs `Txt()`) stopped the script from starting at all = loading-screen flash + reboot
loop, nothing written to the card (2026-09-27). Keep `autorun.brs` pure ASCII (player OS 6.2.63.3).
`SD:/boot_ok.txt` is written at the first line of `Main`: no file after a boot loop = the script never ran.
**Then run `brs_test/run.sh`:** the same syntax check plus the real script logic in a BrightScript
interpreter (`npx brs`, BrightSign objects stubbed) fed STATUS/SHOW/SHOW/END/PLAY/SKIP/PLAY NOW/SHOW FORCE/VOL.
It proves the logic, not the player: SHOW passed here but hung + rebooted the real XT1143 (2026-09-27), so
`SD:/show_trace.txt` records each show-start step on the card.
**OS 6.2 is older than the interpreter:** `.Count()` on an associative array (`{}`) does not exist on the
player (it killed SHOW: trace stopped after step 4) but works in `npx brs`. Only call `.Count()` on arrays /
lists; count AA entries with your own counter.

### Operating rules (the player)

- **Power off before removing or inserting the card.** Pulling the card from a running player leaves the old
  script running in memory: it still answers PING/STATUS over serial, but every video fails
  (`ERR playfile ...` for every file). That cost an hour of false leads on 2026-09-27.
- Stay on **OS 6.2.63.3** for 2026 (works, tested). Updating firmware = new re-test; after Halloween if at all.
- `video_mode` is `""` (auto) on the card and in the repo `show.json` (since `8e057cb`). Set it for the
  projector once known.
- **Partial card updates:** copy only the changed files (e.g. `media/suno/`, the matching transitions,
  `library.json`, `TRACKS.md`), md5 each against `SD Card 2026/`, keep the card's `show.json` unless it is
  meant to change. Save the old files to `/mnt/backup/harvest-of-shadows-card-backups/` first.
  2026-10-27 Suno jaw fix: pre-fix copy at `.../singing-pumpkins-pre-jawfix-2026-10-27/` (rollback steps in
  `docs/singing-pumpkins.md`, face-geometry step 3).

### Garage test, in order (status 2026-09-27)

Steps 1-3 PASS on the desk monitor. Step 4: HA's SKIP over MQTT works end to end; the device in the HA UI
and the "Play now" list are not checked yet. Step 5 not done. SHOW (ESP32 console) PASS: MODE SHOW, opening,
4-5 clips, close-out, back to DJ; second SHOW = BUSY.


1. **Card alone:** microSD (FAT32 or exFAT), copy the *contents* of
   `SD Card 2026/` to its root. Player on a TV over HDMI. Expect the ambient loop
   within ~30 s. A script error shows on screen: photo it, that's the fix list.
2. **Meter checks** (above): HW-027 powered from the ESP32's 3V3/GND only (data wires
   not yet connected), player on.
3. **ESP32:** flash (with Joe at the bench), open the USB monitor, then wire it up.
   Expect `[brightsign] <- PONG`, `pick list: 153 entries`, and a `HB mode=DJ` line
   every 30 s. Type `STATUS`, `SHOW`, `END`, `PLAY 12 NOW` on the USB console.
4. **HA:** the "Singing Pumpkins" device appears; try Play now / Start show / End show.
5. **Live cue:** a real Scarecrow visit: after Burt finishes, the show starts.

## Media findings (2026-09-27 survey of Main 2026)

- **All 1920×1080 H.264**, but mixed frame rates (Suno 24, TSP 30, Atmos 29.97/30)
  and audio (AAC 48 k, AAC 44.1 k, MP3). Staging re-encodes audio to AAC 48 k
  stereo; video is copied untouched.
- **Loudness was all over the place:** Suno ≈ −14 LUFS, TSP −13 to **−1.6**
  (Dead Man's Party clipped hot), Atmos ≈ −20, trick-or-treat ≈ −21.6. Staging
  normalizes everything to **−16 LUFS** (true peak −1.5) with 0.3 s audio fades,
  so songs no longer jump in volume and cuts don't pop.
- **Face positions:** Atmos, trick-or-treat clips, and the buffers share one
  layout (left x≈72–616, centre x≈780–1240, right x≈1444–1812; y≈410–836).
  TSP sits in the same three zones but its faces are ~1.35x the Atmos size (Joe: "giant"). **Fixed
  2026-09-27:** `build_card.py` `FACE_SHRINK = {"TSP": 0.75}` scales each face in place around its own
  centre (x 346/1012/1628), re-encoded; Joe approved on the projection. The TSP resting faces in the
  transitions are shrunk the same way. **Suno faces are
  centred in the same zones but only ~70% of the Atmos size** (left x≈156–540,
  y≈492–752), because the hand-marked boxes include margin around the face art.
  Fixable by re-rendering Suno with larger target boxes (`_build_script.py`, ~1 h).
- The three JOLJ3 "Extra Buffer Files" folders are identical copies; one trio
  buffer is used. Solo-pumpkin buffers are left out (wrong layout).
- Suno had no bridge footage: `build_card.py` generates a 3 s fade-in and
  fade-out of a resting Suno frame (`media/suno/_bridge_{in,out}.mp4`).
- "The Singing Pumpkin" has ghosts flying across the whole frame, and "Bad Moon
  Rising" has full-frame flashes. That's intended art, but it will spill off
  the pumpkins.
