#include "es8388.h"
#include <Arduino.h>
#include <Wire.h>

// ── register map ────────────────────────────────────────────────────────────
#define ES_ADDR            0x10
#define ES_CONTROL1        0x00
#define ES_CONTROL2        0x01
#define ES_CHIPPOWER       0x02
#define ES_ADCPOWER        0x03
#define ES_DACPOWER        0x04
#define ES_CHIPLOPOW1      0x05
#define ES_CHIPLOPOW2      0x06
#define ES_ANAVOLMANAG     0x07
#define ES_MASTERMODE      0x08
#define ES_DACCONTROL1     0x17
#define ES_DACCONTROL2     0x18
#define ES_DACCONTROL3     0x19
#define ES_DACCONTROL4     0x1a
#define ES_DACCONTROL5     0x1b
#define ES_DACCONTROL16    0x26
#define ES_DACCONTROL17    0x27
#define ES_DACCONTROL18    0x28
#define ES_DACCONTROL19    0x29
#define ES_DACCONTROL20    0x2a
#define ES_DACCONTROL21    0x2b
#define ES_DACCONTROL24    0x2e   // LOUT1 volume
#define ES_DACCONTROL25    0x2f   // ROUT1 volume
#define ES_DACCONTROL26    0x30   // LOUT2 volume
#define ES_DACCONTROL27    0x31   // ROUT2 volume

static Es8388Out s_out = ES_OUT_ALL;

static bool wr(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool rd(uint8_t reg, uint8_t& val) {
    Wire.beginTransmission(ES_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)ES_ADDR, 1) != 1) return false;
    val = Wire.read();
    return true;
}

bool es8388_init(int sda, int scl, Es8388Out out) {
    s_out = out;
    Wire.end();
    Wire.begin(sda, scl, 100000);
    delay(5);

    // codec present?
    Wire.beginTransmission(ES_ADDR);
    if (Wire.endTransmission() != 0) return false;

    bool ok = true;
    // --- core: slave mode, power-cycle the digital blocks ---
    ok &= wr(ES_MASTERMODE, 0x00);      // ESP32 is I2S master, codec is slave
    ok &= wr(ES_CHIPPOWER, 0xFF);       // power down DEM + STM while we configure
    ok &= wr(ES_DACCONTROL21, 0x80);    // DAC/ADC share the same LRCK

    ok &= wr(ES_CONTROL1, 0x05);        // enable ref, softramp
    ok &= wr(ES_CONTROL2, 0x40);        // power up analog + Ibias

    // --- ADC block OFF (narrator never records) ---
    ok &= wr(ES_ADCPOWER, 0xFF);        // full ADC power-down

    // --- DAC block ---
    ok &= wr(ES_DACPOWER, 0x3C);        // DAC L/R on, LOUT1/ROUT1 + LOUT2/ROUT2 enabled
    ok &= wr(ES_DACCONTROL1, 0x18);     // I2S format, 16-bit
    ok &= wr(ES_DACCONTROL2, 0x02);     // MCLK / LRCK = 256
    ok &= wr(ES_DACCONTROL3, 0x00);     // unmute
    ok &= wr(ES_DACCONTROL4, 0x00);     // DAC L digital volume = 0 dB
    ok &= wr(ES_DACCONTROL5, 0x00);     // DAC R digital volume = 0 dB

    // output mixer: route DAC straight to the output mixer, no analog bypass
    ok &= wr(ES_DACCONTROL16, 0x00);    // LMIXSEL/RMIXSEL = LIN1/RIN1 (unused, ADC off)
    ok &= wr(ES_DACCONTROL17, 0x90);    // LI2LO off, LD2LO on (DAC -> left out mixer)
    ok &= wr(ES_DACCONTROL18, 0x38);
    ok &= wr(ES_DACCONTROL19, 0x38);
    ok &= wr(ES_DACCONTROL20, 0x90);    // RD2RO on (DAC -> right out mixer)

    // start with output volume low; caller sets the real level
    ok &= wr(ES_DACCONTROL24, 0x00);
    ok &= wr(ES_DACCONTROL25, 0x00);
    ok &= wr(ES_DACCONTROL26, 0x00);
    ok &= wr(ES_DACCONTROL27, 0x00);

    ok &= wr(ES_CHIPPOWER, 0x00);       // power up DEM + STM — codec live

    return ok;
}

bool es8388_set_volume(uint8_t pct) {
    if (pct > 100) pct = 100;
    uint8_t v = (uint8_t)((pct * 33 + 50) / 100);   // 0..33
    bool ok = true;
    if (s_out == ES_OUT_ALL || s_out == ES_OUT1) {
        ok &= wr(ES_DACCONTROL24, v);
        ok &= wr(ES_DACCONTROL25, v);
    }
    if (s_out == ES_OUT_ALL || s_out == ES_OUT2) {
        ok &= wr(ES_DACCONTROL26, v);
        ok &= wr(ES_DACCONTROL27, v);
    }
    return ok;
}

bool es8388_mute(bool mute) {
    return wr(ES_DACCONTROL3, mute ? 0x04 : 0x00);
}

void es8388_dump_regs() {
    Serial.println(F("  ES8388 regs 0x00..0x34:"));
    for (uint8_t r = 0; r <= 0x34; r++) {
        uint8_t v = 0xFF;
        bool ok = rd(r, v);
        Serial.printf("    [0x%02X] = %s0x%02X\n", r, ok ? "" : "(rd fail) ", v);
    }
}
