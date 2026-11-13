#pragma once
// =============================================================================
// ESP32-A1S (AI-Thinker Audio Kit v2.2) — pin map + node identity
// =============================================================================
// Bench-confirmed 2026-09-06 (props/scarecrow-narrator-a1s/bench/):
//   SoC ESP32-D0WD-V3 rev 3.1 · 4 MB flash · 4.00 MB PSRAM
//   Codec  : ES8388 @ I2C 0x10   (NOT AC101)
//   DIP    : 1,2,3 ON / 4,5 OFF  -> 1-bit SDMMC bus on CLK14/CMD15/D0 2,
//            DAT3 (GPIO13) + its pull-up present, GPIO12 isolated (free).
//   MAC    : AA:BB:CC:DD:EE:FF = MAC_SKELETON_A1S (NodeID 0x07, "Burt" — follower)
// =============================================================================

// ── I2S (ESP32 = master, ES8388 = slave; ESP drives MCLK) ─────────────────────
#define A1S_I2S_MCLK   0    // also BOOT strap — never wire anything else here
#define A1S_I2S_BCLK   27
#define A1S_I2S_LRCK   25
#define A1S_I2S_DOUT   26   // ESP -> codec (playback)
#define A1S_I2S_DIN    35   // codec -> ESP (unused by narrator; input-only pin)

// ── I2C (codec control only) ─────────────────────────────────────────────────
#define A1S_I2C_SDA    33
#define A1S_I2C_SCL    32
#define A1S_ES8388_ADDR 0x10

// ── Power amp (on-board NS4150, drives the 3 W speaker terminals) ─────────────
// Narrator design: feed a POWERED speaker + PicoTalk from LINE OUT / headphone
// jack (post-DAC, independent of this amp) and hold PA_EN LOW (less noise, less
// current). Drive HIGH only for bench listening on the board's speaker pads.
#define A1S_PA_EN      21

// ── microSD (1-bit SDMMC — see DIP note above) ───────────────────────────────
#define A1S_SD_CLK     14
#define A1S_SD_CMD     15
#define A1S_SD_D0      2
#define A1S_SD_CD      34   // card-detect, input-only, optional

// ── Free GPIO for prop hardware (bench-measured 2026-09-06) ──────────────────
// 22, 23, 18, 5 : clean, header-accessible, output-capable  -> 2x servo + LED eyes + spare
// 19            : usable but idles HIGH (populated ~10k pull-up)
// 12            : free (DIP-isolated) but a boot strap — reserve, don't use unless needed
#define A1S_FREE_LED_EYES   22
#define A1S_FREE_SERVO_A    23
#define A1S_FREE_SERVO_B    18
#define A1S_FREE_SPARE      5

// ── Node identity ───────────────────────────────────────────────────────────
// This board == Burt (Skeleton). MAC AA:BB:CC:DD:EE:FF bench-confirmed 2026-09-06.
#define A1S_NODE_ID          0x07   // NODE_SKELETON_A1S
#define A1S_NODE_NAME        "burt"
