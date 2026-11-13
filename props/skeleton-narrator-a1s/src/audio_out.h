#pragma once
// =============================================================================
// I2S output — ESP32 as I2S master feeding the ES8388 (MCLK on GPIO0)
// =============================================================================
// Thin wrapper over the ESP-IDF legacy i2s driver (driver/i2s.h). Legacy is
// deprecated but is the simplest path that supports MCLK output on GPIO0, which
// the ES8388 on this board needs. 16-bit stereo frames only; mono sources are
// duplicated L=R by audio_out_write_mono().
//
// This is the structural playback sink for Steps 3-5: SD WAV reader and the
// HTTP MIDDLE fetch both end up calling audio_out_write*().
// =============================================================================

#include <stdint.h>
#include <stddef.h>

bool   audio_out_begin(uint32_t sample_rate);
void   audio_out_end();
void   audio_out_set_rate(uint32_t sample_rate);      // switch rate between files
uint32_t audio_out_rate();

// Blocking writes. Return frames/samples actually queued.
size_t audio_out_write(const int16_t* interleaved_stereo, size_t frames);
size_t audio_out_write_mono(const int16_t* mono, size_t samples);

void   audio_out_silence_ms(uint32_t ms);             // push zeros (keeps codec clocked)
void   audio_out_flush();                             // wait for DMA to drain

// Bench helper — synthesize a sine straight to the output.
void   audio_out_tone(uint32_t freq_hz, uint32_t ms, float amplitude /*0..1*/);
