# SD card format — per node type

**Status: 2026-09-07.** How audio gets onto each kind of playback node, and
what each one actually needs from the card. Different hardware, different rules —
verified per type, not assumed.

Quick answer:

| Node type | Hardware | Addressing | Copy order matters? | Status |
|---|---|---|---|---|
| **A1S narrators** — Scarecrow, Burt | ESP32-A1S + `SD_MMC` (custom firmware) | **explicit file path** + `index.csv` | **No** | ✅ confirmed (firmware read) |
| **YX5300 MP3 nodes** — Spooky Tree, (future) Patch L/R | YX5300 serial module + `MD_YX5300` lib | track *number* → **FAT directory order** | **Yes (likely)** | ⚠️ needs bench confirmation on the actual modules |
| Media players — Zombie Container (MediaWiz), Singing Pumpkins (BrightSign XT1143), Stain Glass | dedicated video/media engine, ESP32 only arms/triggers | player-specific (playlist / GPIO / serial) | player-specific | out of scope here — see each prop's own doc |

Filesystem for **all** SD cards in the project: **FAT32.** Not exFAT (the
ESP32 `SD_MMC` / FATFS stack and most YX5300 clones don't read it). Format with
the official **SD Card Formatter** (sdcard.org), full format, default allocation
unit. 8–16 GB card. FAT16 is fine only for ≤ 2 GB cards.

---

## A1S narrators (Scarecrow, Burt) — CONFIRMED order-independent

The A1S runs custom firmware (`props/{scarecrow,skeleton}-narrator-a1s/src/pools.cpp`).
It does **not** use a serial MP3 module. It mounts the card as a FAT filesystem
(`SD_MMC`, 1-bit mode, pins CLK/CMD/D0 = GPIO 14/15/2) and opens files by
**explicit path**:

```c
File f = SD_MMC.open("/scarecrow/index.csv", FILE_READ);          // count table
...
snprintf(path, "/scarecrow/%s/%s/%03u.wav", seg, bucket, idx);    // e.g. .../rotld_scary/007.wav
File f = SD_MMC.open(path, FILE_READ);
```

**Therefore:**
- **Copy order is irrelevant.** FAT allocation order, the order files were
  written, "track 1 = first file added" — none of it applies. The firmware
  computes a path and opens that path.
- **Filenames must be `000.wav` … `(count-1).wav`**, zero-padded to 3 digits.
  The firmware picks `idx = esp_random() % count` (range `0 … count-1`) and opens
  `%03u.wav`, so the pool must start at `000.wav` with no gap before `count-1`.
  (A missing file in the middle is survived — it walks forward a few indices —
  but the count in `index.csv` must be right.)
- **`index.csv` at the card root of the node's folder** gives the per-bucket
  count. Format:
  ```
  segment,bucket,count
  intro,corn_scary,50
  intro,corn_friendly,50
  ... (all 6 intro buckets, all 6 outro buckets)
  outro,pumpkins_friendly,50
  bridge,generic,30
  pool_version,20260907,
  ```
  `#`-comment lines and a `pool_version,<n>,` line are allowed.
- **Sample rate: anything.** `wav_play()` reconfigures the ES8388 per file, so
  16 kHz / 22.05 kHz / 24 kHz all work. The staging build uses **24 kHz mono
  16-bit PCM WAV** (still tiny — ~0.5 GB/card). Use `--sr 16000` to halve it.
- **Per-board folder prefix** — baked into each firmware, already correct:
  - Scarecrow firmware → `/scarecrow/…`
  - Burt firmware → `/burt/…` (re-pointed from `/scarecrow/` when cloned — see
    `props/skeleton-narrator-a1s/BUILD_LOG.md`)

### On-card layout — Intro / Middle / Closing per theme

This **is** the structure Joe wants, in the firmware's names:

```
/scarecrow/                    <- or /burt/ on Burt's card
  index.csv
  intro/                       <-  "INTRO"
    corn_scary/      000.wav … 049.wav
    corn_friendly/   000.wav … 049.wav
    rotld_scary/     …
    rotld_friendly/  …
    pumpkins_scary/  …
    pumpkins_friendly/ …
  outro/                       <-  "CLOSING"  (haunt/theatre term = "outro")
    corn_scary/ … pumpkins_friendly/          (same 6 theme folders)
  bridge/                      <-  "MIDDLE"   (on-card fallback only, see note)
    generic/         000.wav … 029.wav
```

- **INTRO** = `intro/`. **CLOSING** = `outro/` (same thing — "outro" is the term
  in `docs/_history/dialogue-architecture.md` and the firmware; think of it as "closing").
- **MIDDLE** = `bridge/generic/`. The *real* MIDDLE line is assembled per
  visitor on the bridge and streamed over WiFi (`docs/CURRENT-BUILD.md` §2,
  a1s doc §2.2); it is **never on the card**. The card's `bridge/generic/`
  pool is the pre-recorded fallback that plays only when the live pipeline is
  down (a1s doc §2.3 ladder). It is **30 Scary, theme-agnostic lines** — one
  flat folder, deliberately *not* split by theme or tone (Joe's decision
  2026-09-06).
- **Themes** are the sub-folders (`rotld_scary`, `pumpkins_friendly`, …), so
  each theme's Intro and Closing content is one folder you can drop files into.
  The 6 = 3 themes × 2 tones (Scary / Friendly) — the tone lives in the folder
  name.
- Each bucket folder also gets a `MANIFEST.txt` (card file `NNN.wav` ↔ source
  line `NNN+1`) so a bad line is easy to find and re-roll.

### Building the card

Rendered pools live in `orchestration/voice/out/pools/`. The staging tree —
ready to copy to the card root — is built by:

```bash
cd orchestration/voice
bash scripts/build_a1s_card.sh scarecrow ./sd-card --show     # -> sd-card/scarecrow/
bash scripts/build_a1s_card.sh burt      ./sd-card --show     # -> sd-card/burt/
# options:  --show (use _show.wav: denoise+bed, loudnorm already applied — the
#           production default since 2026-09-07)   --sr 16000 (smaller)
#           --raw (raw XTTS, no FX)   --root NAME (override the on-card folder)
```

`--show` reads `NN_show.wav` (the post-processed files —
`docs/audio-post-processing.md`) and skips the re-loudnorm. Without `--show` it
reads `NN_raw_fx.wav` and applies `loudnorm I=-18`. Either way: → mono → `<sr>`
Hz 16-bit → renumbered `000.wav…`, plus `index.csv` and per-bucket
`MANIFEST.txt`. `orchestration/voice/sd-card/` is gitignored (audio, ~1 GB).

**Writing to a physical card — `scripts/flash_a1s_card.sh` (needs sudo):**

```bash
# fresh card: wipe + FAT32 + copy  (destructive; interactive "ERASE sdX" confirm)
sudo bash scripts/flash_a1s_card.sh /dev/sdX ./sd-card/scarecrow

# update an already-formatted card in place  (replaces /<root>/ only, keeps the
# filesystem; interactive "UPDATE sdX" confirm) -- use after a re-treat
sudo bash scripts/flash_a1s_card.sh --copy-only /dev/sdX ./sd-card/burt
```

Both modes guard the device (removable, sane size, not `sda`/`sdb`/`sdc`/the
root disk) and verify after copy (wav count, `index.csv` present, 3 random md5s).
Or do it by hand: format FAT32, then **drag the whole `scarecrow/` folder** onto
the card so it lands as `<card>/scarecrow/index.csv` (folder and all — not just
the contents). Each staging folder has its own `README-COPY-TO-SD.txt`.

### Verify on the board

Serial monitor, command `d` dumps the pool table it read from `index.csv`;
command `c` runs a cycle (INTRO → MIDDLE → OUTRO). Expect:
`[pools] SD mounted … [pools] index.csv: 13 rows … [pools] play /scarecrow/intro/…`.

---

## YX5300 MP3 nodes (Spooky Tree; future Patch L/R) — ⚠️ TEST THE REAL MODULES

**Spooky Tree** uses a YX5300 serial MP3 module (`Serial2` GPIO 16/17 @ 9600,
`MD_YX5300` library). The planned **Patch Left / Patch Right** spatial-audio
nodes are Yard-Boombox-class and will most likely use the same module family
(YX5300 / YX6300 / clones).

**Why this is an open question, not a datasheet lookup:** the spooky_tree
firmware calls `s_mp3.playTrack(track)` with an integer track number. In
`MD_YX5300` that sends the module's *play-index* command (`0x03`). On the
**common YX5300**, the "index" is **the file's position in the SD card's FAT
directory table** — i.e. **the order files were physically written to the card**,
*not* the filename. So `playTrack(1)` = "the first file copied", `playTrack(2)` =
"the second file copied", regardless of what they're named. Some clones honour
numeric filename order instead; some need `/01/001.mp3` folder addressing via a
different command (`0x0F` `playSpecific`). **The modules on hand may do any of
these** — and the DIP-switch question showed that trusting the generic spec here
is how you get bitten.

The firmware's current assumption (`props/spooky-tree/src/mp3_player.h`):

```
TRACK_AMBIENT  = 1     tree ambient loop        -> must be the 1st file on the card
TRACK_ACTION   = 2     elk scream (PIR)         -> 2nd
TRACK_THEME_CORN  = 7  / _ROTLD = 8 / _MPUMP = 9   -> 7th / 8th / 9th
```

Spooky Tree is deployed and working, so *its* card is already ordered correctly —
but that ordering has never been written down, and the Patch nodes are new.

### Bench test — do this with the actual module before loading a real card

Same method as the DIP-switch resolution: write known files, observe real
behaviour, record it.

1. **Format** a fresh card FAT32 (SD Card Formatter, full).
2. **Copy, one file at a time, in this order** (so FAT order and name order
   *disagree* on purpose):
   - `zzz_first.mp3`  — a 2 s clip that says **"one"**
   - `aaa_second.mp3` — says **"two"**
   - `mmm_third.mp3`  — says **"three"**
   - then a folder `01/` containing `001.mp3` ("folder one one") and
     `002.mp3` ("folder one two")
3. Flash a 20-line sketch (or use the spooky_tree serial console) that on key
   press sends, with ≥ 250 ms spacing:
   - `playTrack(1)`, `playTrack(2)`, `playTrack(3)`
   - `playSpecific(1,1)`, `playSpecific(1,2)` (if the lib build exposes it)
4. **Record what you hear** for each command. Determine:
   - Does `playTrack(1)` play `zzz_first` ("one") → **FAT/copy order**; or
     `aaa_second` ("two") → **alphabetical filename order**; or something else?
   - Does `playSpecific(1,1)` work at all → **folder addressing available**?
5. Write the result into this section and into
   `props/spooky-tree/docs` / `IMPROVEMENTS.md`. If it's copy-order, the
   card-loading procedure for every YX node becomes: **format, then copy files
   in strict track-number order, verifying each before the next** (a
   `copy_yx_card.sh` that `cp`s one file and `sync`s between each is the safe
   way).

### Interim rule (until tested)

Treat YX5300 nodes as **copy-order-dependent**: format fresh, copy in exact
track order, one at a time, `sync` between each, never bulk-copy or drag a
folder. Name files `001_ambient.mp3`, `002_action.mp3`, … so the intended order
is also the name order — that covers both possible behaviours.

**This does not affect the A1S narrators at all** — they're a different
mechanism (above).

---

## Media players (Zombie Container, Singing Pumpkins, Stain Glass)

These props recently moved to dedicated media engines (`docs/fleet-communication-tiers.md`
classification table, 2026-09-06): Zombie Container → MediaWiz 1080, Singing
Pumpkins → BrightSign XT1143, Stain Glass → candidate BrightSign 4K1142-WW. The
ESP32 on each only **arms / triggers** (dry relay contacts or serial); the media
engine owns its own storage and file conventions. Their card formats are
**per-player and out of scope for the narrator voice pools** — document them in
each prop's own doc (`docs/zombie-container.md`, `docs/singing-pumpkins.md`,
`docs/stain-glass-window.md`) when those get built.

---

## References

- `props/scarecrow-narrator-a1s/src/pools.{h,cpp}` — the A1S path/index lookup
- `props/skeleton-narrator-a1s/BUILD_LOG.md` — Burt's `/burt/` prefix re-point
- `orchestration/voice/scripts/build_a1s_card.sh` — the staging build
- `docs/a1s-board-and-audio-architecture.md` §2.1–2.4 — pool design + fallback ladder
- `props/spooky-tree/src/mp3_player.{h,cpp}` — the YX5300 `playTrack()` usage
- memory `feedback_incremental_hardware_testing` — "test hardware one confirmed
  step at a time, never blind" (this is why the YX answer is a test, not a claim)
