# Learned IR codes — LED controller remote

Captured 2026-09-30 with the ir-learner (VS1838-style 38kHz receiver on GPIO16).
All NEC, 32-bit. "Verified" = replayed from the TX module and the LEDs responded.

| Button | NEC code | Verified |
|---|---|---|
| RED | 0xFF1AE5 | YES (2026-09-30) |
| GREEN | 0xFF9A65 | YES (2026-09-30) |
| AQUA (swamp, w/ GREEN) | 0xFF8A75 | YES (2026-09-30) |
| WHITE (bright) | 0xFF22DD | YES (2026-09-30) |
| FLASH (strobe, w/ WHITE) | 0xFFD02F | YES (2026-09-30) |
| POWER (toggle) | 0xFF02FD | not used (toggle; never sent by the show node) |
| BRIGHT UP | 0xFF3AC5 | YES (2026-09-30) |

## Not captured: standard 44-key table

Replay-VERIFIED on the controller 2026-09-30: QUICK 0xFFE817, BRIGHT DOWN 0xFFBA45,
DIY1 0xFF30CF (Joe mixed DIY1 to black = the show node's "off"; survives a
controller power cycle), BLUE 0xFFA25D. The rest below are still UNVERIFIED.

All 7 captures above match the common 44-key RGB remote's NEC table exactly
(address 0x00), so the rest of the buttons most likely follow it too. Nothing
below has been captured from this remote or replay-tested on this controller.
Capture a button, or replay it and see the LEDs respond, before a show build
depends on it.

| Button | NEC code | | Button | NEC code |
|---|---|---|---|---|
| BRIGHT UP | captured, see above | | QUICK | 0xFFE817 |
| BRIGHT DOWN | 0xFFBA45 | | SLOW | 0xFFC837 |
| PLAY/PAUSE | 0xFF827D | | AUTO | 0xFFF00F |
| BLUE | 0xFFA25D | | JUMP3 | 0xFF20DF |
| RED UP / DOWN | 0xFF28D7 / 0xFF08F7 | | JUMP7 | 0xFFA05F |
| GREEN UP / DOWN | 0xFFA857 / 0xFF8877 | | FADE3 | 0xFF609F |
| BLUE UP / DOWN | 0xFF6897 / 0xFF48B7 | | FADE7 | 0xFFE01F |
| DIY1-3 | 0xFF30CF / 0xFFB04F / 0xFF708F | | DIY4-6 | 0xFF10EF / 0xFF906F / 0xFF50AF |

## TX module bench results (2026-09-30)

Dorhea 38kHz IR transmitter modules, VCC 3V3, S on ESP32 GPIO17. Each test
was 60x NEC sends over ~15 s at the LED controller.

- #1: FAIL on 2 bursts. The indicator LED blinked but nothing reached the controller.
- #2: PASS first try. #3: PASS first try.
- #4: failed first burst, then passed. Every failure traced to AIM.
- Listing specs: 20 degree emission angle, ~1.3 m range at 5V. The beam is narrow,
  so the LED MUST point straight at the controller's IR eye. Being off-angle is
  indistinguishable from a dead module.
- For the ESP-01S build: fix the LED mechanically, close to and pointed straight
  at the IR eye. Module VCC goes on 5V for range. Before connecting S to a 3.3V
  GPIO, confirm S does not float at 5V (see ESP-01S notes).
