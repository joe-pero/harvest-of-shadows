# patch-audio-right

Spatial audio patch node — **RIGHT channel** of the pre-mixed theme soundscape.
`NODE_PATCH_RIGHT` (0x10). ESP32 + YX5300 + SD + one powered speaker. Tier 1,
receive-only.

**2026-10-26: `1fa5794` BUILT, not flashed** — see `../patch-audio-left/README.md` and CURRENT-BUILD #50.

**Identical firmware to `../patch-audio-left/`.** This project's `platformio.ini`
sets `src_dir = ../patch-audio-left/src` — there is exactly one source tree. The
only differences between the two nodes are the SD-card content (right-channel
mixes) and `THIS_NODE`.

```
pio run -e patch_right -t upload
pio device monitor -e patch_right
```

Full reference: `docs/patch-audio-nodes.md`.

⚠ MP3 board assumed YX5300 — confirm the silkscreen. MAC not yet recorded —
update `MAC_PATCH_RIGHT` from the boot log.

Status 2026-09-06: compiles clean, not bench-tested.

**2026-10-01: REBUILT on the Spooky Tree's base** (Joe: "same concept as the tree; the default logic
for these mp3 players"): shared `SafeYX5300` driver (ambient replayed on every wrap), `ShowFollower.h`
(theme once on the Scarecrow's SHOW_ACTIVE, then back to ambient), black box on the chip
(`tools/blackbox_pull.sh`), self-heal reboot (idle only), 20 s settle on a cold boot then MP3 then radio,
loop watchdog, reset reason, noise-proof serial, no OTA, NODE_ONLINE at boot only. Builds clean;
**not bench-tested, never flashed.** MAC is logged at boot (no constant yet). Bench list:
`docs/patch-audio-nodes.md`.
