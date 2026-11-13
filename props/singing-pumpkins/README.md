# singing-pumpkins

**SCAFFOLD ONLY (2026-09-06) — not a build.** `NODE_SINGING_PUMPKINS` (0x05),
Tier 1 receive-only.

Holds the 2026 ESP-NOW listen structure + the active/idle mode switch **as
stubs**. The real node is a partial port of `Projects/Singing pumpkins V2/`.

**Media engine (2026-09-06): BrightSign XT1143** — replaces the 2025 DV68 /
MediaWiz Sprite. Serial: 3.5 mm TRS jack (Tip=RX, Ring=TX, Sleeve=GND), 115200
8N1, via a MAX3232 3.5 mm adapter into a spare ESP32 `HardwareSerial`.

```
pio run -e singing_pumpkins        # compiles; do not flash as-is
```

| file | state |
|---|---|
| `espnow_rx.{h,cpp}` | ✅ standard 2026 broadcast RX |
| `brightsign.{h,cpp}` | ⚠ stub — logs intent; confirmed XT1143 serial details in the header comment |
| `main.cpp` | ⚠ scaffold — theme latch + active/idle switch; relays / IR / song deck / jukebox are `TODO(port)` |

Full plan + open items (BrightSign presentation authoring, Fisher-Yates song
deck, **jukebox touch-pad location**): `docs/singing-pumpkins.md`.
