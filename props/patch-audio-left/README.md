# patch-audio-left

Spatial audio patch node — **LEFT channel** of the pre-mixed theme soundscape.
`NODE_PATCH_LEFT` (0x0F). ESP32 + YX5300 + SD + one powered speaker. Tier 1,
receive-only.

**This directory holds the source tree for BOTH patch nodes** —
`../patch-audio-right/` builds the same `src/` with `THIS_NODE=NODE_PATCH_RIGHT`.

```
pio run -e patch_left -t upload
pio device monitor -e patch_left
```

Full reference: `docs/patch-audio-nodes.md`. Modelled on `props/spooky-tree/`.

⚠ MP3 board assumed YX5300 — **confirm the silkscreen before flashing.**
⚠ MAC not yet recorded — read it off the boot log, update `MAC_PATCH_LEFT`.

**2026-10-26: `1fa5794` BUILT, not flashed (both Left and Right)** — the shared follower no longer guesses the theme: a missed START_SCENE means sitting the show out, not playing the last theme (CURRENT-BUILD #50). Patch L/R were off on 10-26.

Status 2026-09-06: firmware written, **compiles clean**, not bench-tested (no
hardware on hand).

**2026-10-01: REBUILT on the Spooky Tree's base** (Joe: "same concept as the tree; the default logic
for these mp3 players"): shared `SafeYX5300` driver (ambient replayed on every wrap), `ShowFollower.h`
(theme once on the Scarecrow's SHOW_ACTIVE, then back to ambient), black box on the chip
(`tools/blackbox_pull.sh`), self-heal reboot (idle only), 20 s settle on a cold boot then MP3 then radio,
loop watchdog, reset reason, noise-proof serial, no OTA, NODE_ONLINE at boot only. Builds clean;
**not bench-tested, never flashed.** MAC is logged at boot (no constant yet). Bench list:
`docs/patch-audio-nodes.md`.
