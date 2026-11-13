#pragma once
// =============================================================================
// Hosyond 4.0" ESP32-32E (ST7796S) — pin map for the touchpad controller
// =============================================================================
// Display/touch/SD/RGB assignments are the board's fixed wiring
// (lcdwiki.com/4.0inch_ESP32-32E_Display). The relay + sense pins below are the
// draft choices for THIS node — free, output-capable GPIO left after the display.
// =============================================================================

// ── consumed by the board (fixed) ──────────────────────────────────────────
//  LCD ST7796S : CS 15 · DC 2 · SCLK 14 · MOSI 13 · MISO 12 · BL 27   (reset = EN)
//  Touch XPT2046: CS 33 · IRQ 36 · shares 14/13/12
//  microSD      : CS 5 · SCLK 18 · MISO 19 · MOSI 23   (free if slot unused)
//  RGB LED      : see below (common-anode, ACTIVE-LOW)
//  audio enable : 4      battery ADC : 34      BOOT/RESET : 0 / EN

// ── LCD backlight — held OFF until the touch UI is built (no display code yet) ─
#define LCD_BL_PIN  27

// ── RGB indicator LED (common-anode → drive LOW = on; use LEDC inverted) ────
#define LED_R_PIN   22
#define LED_G_PIN   16
#define LED_B_PIN   17

// ── relay outputs — driven off the on-board "I2C peripheral interface" (P4) ──
// CONFIRMED 2026-09-07 against the E32R40T user manual (lcdwiki), Fig 3.10/3.13:
//   P4 is a 4-pin 1.25 mm header — pin 1 VCC3V3 · pin 2 GND · pin 3 IIC_SDA ·
//   pin 4 IIC_SCL. IIC_SDA = GPIO32, IIC_SCL = GPIO25. Both are ordinary
//   output-capable GPIO (NOT the input-only IO35/IO39 "Expand input pin" JP3).
//   Each line carries a 10 kΩ pull-up to 3.3 V (R34 on SDA, R33 on SCL) — the
//   manual says remove them for plain-IO use, but for an active-LOW relay input
//   the pull-up is harmless and helpfully holds the relay OFF through boot/reset.
//   Joe's harness wires (this board type): black=GND, red=VCC3V3, and the two
//   signal wires (yellow/green) = SDA/SCL — verify GND/VCC with a meter before
//   powering, then confirm which signal wire is GPIO32.
//
// CONFIRMED 2026-09-19 (Joe) — relay is a single-board JQC-3FF-S-Z 5 V module
// (VCC/GND/IN header, plain GPIO trigger, no I2C device). Joe's wiring: red→VCC,
// black→GND, green(GPIO32)→IN, yellow(GPIO25) spare. ⚠ UNRESOLVED: Joe's note
// calls the red wire "5V", but this header's pin 1 is documented above as
// VCC3V3 (3.3 V LDO output) — if a meter confirms 3.3 V, the module's VCC needs
// a separate 5 V feed instead (tie grounds together, drive IN from GPIO32 as
// planned; opto/logic side runs fine at 3.3 V on most modules regardless of
// VCC level). Don't power the relay until this is metered.
//
// RELAY_DMX  → Zombie Container PicoDMX (Fright Ideas) trigger input. Momentary
//   closure = a simulated button push (replaces the old RF-remote relay), dry
//   contact NO+COM into the PicoDMX trigger. Pulse width CONFIRMED 1 s (main.cpp
//   PICODMX_PULSE_MS). Single relay — no second relay in this chain.
#define RELAY_DMX_PIN    32     // = P4 pin 3, IIC_SDA
// RELAY_AUX  → 2nd relay / "media player" trigger — device UNCONFIRMED (ASK JOE).
#define RELAY_AUX_PIN    25     // = P4 pin 4, IIC_SCL

// active level of the relay module inputs (many cheap boards are active-LOW)
#define RELAY_ACTIVE_LEVEL   LOW

// ── optional sense inputs (input-only pins) ────────────────────────────────
#define SENSE_A_PIN     35   // e.g. PicoDMX "running"/status output, if it has one
#define SENSE_B_PIN     39   // e.g. a hardware arming key

// ── node identity ──────────────────────────────────────────────────────────
#define NODE_NAME   "touchpad-ota"
