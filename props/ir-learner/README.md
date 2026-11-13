# IR learner (bench tool)

**2026-09-30: this ESP32 is now the DMX light controller (`props/zombie-dmx-light/`); the IR modules came off.**

**2026-10-01: the learner also runs on the 2025 Singing Pumpkins ESP32 (MAC `AA:BB:CC:DD:EE:FF`, receiver S on RX2/GPIO16). Used to capture the Hitachi projector POWER code (`docs/projector-ir.md`).**

Classic ESP32 DevKit (ESP32-D0WD-V3, MAC `AA:BB:CC:DD:EE:FF`) + Dorhea 38 kHz IR
receiver/transmitter modules. It was used on 2026-09-30 to capture the Zombie
Container LED controller's remote and to tune the effects that now run in
`props/zombie-ir-lights/`. It is not a show node.

- Wiring: RX module S to **RX2 (GPIO16)**, TX module S to **TX2 (GPIO17)**, both on 3V3/GND.
- Captured codes + verification status: **`codes.md`**.
- Serial (115200): the dump prints every received code. `n<hex>` sends NEC,
  `r`/`R` replays. Presets: `S` swamp, `A` red alert, `W` white strobe, `Z` zombie.
  `f`/`a`/`g`/`w`/`z` tune live. `x` stops.
- Flash: `pio run -e ir_learner -t upload --upload-port /dev/ttyUSB0` (115200: the
  default speed hit a stub checksum error on this board).
- Opening the serial port resets this board, so anything held in RAM (the last
  capture) is lost. That's why `n<hex>` exists.
