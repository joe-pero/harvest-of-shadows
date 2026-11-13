#pragma once
// =============================================================================
// Minimal ES8388 codec driver — DAC playback path only (narrator never records)
// =============================================================================
// Register sequence adapted from the ES8388 User Guide example and
// vanbwodonk/es8388arduino (proven on this exact AI-Thinker board). Trimmed to
// the DAC-only path: ADC block powered down to save current and switching noise.
//
// The ESP32 I2S peripheral must already be producing MCLK on GPIO0 (or be about
// to) — es8388_init() only touches the I2C control bus.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

// Which physical output pair to enable.
enum Es8388Out {
    ES_OUT1 = 0,   // LOUT1 / ROUT1  — the AI-Thinker "headphone" / line jack
    ES_OUT2,       // LOUT2 / ROUT2  — the on-board Class-D amp input
    ES_OUT_ALL,    // both
};

// Bring up the I2C bus (Wire) on SDA/SCL and run the DAC init sequence.
// Returns false if the codec doesn't ACK at 0x10 or any register write fails.
bool es8388_init(int sda, int scl, Es8388Out out = ES_OUT_ALL);

// 0..100 -> mapped to the codec's 0..33 LOUT/ROUT analog volume steps.
// One call at start-up; the narrator normalises its pools and never rides this.
bool es8388_set_volume(uint8_t pct);

// Digital DAC mute (click-free-ish; use around track boundaries if needed).
bool es8388_mute(bool mute);

// Dump all 53 registers to Serial — bench diagnostics.
void es8388_dump_regs();
