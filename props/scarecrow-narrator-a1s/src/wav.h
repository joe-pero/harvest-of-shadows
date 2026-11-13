#pragma once
// =============================================================================
// Minimal WAV reader — canonical RIFF/PCM, streamed to the I2S output
// =============================================================================
// Handles the render pipeline's output format: PCM 16-bit LE, mono or stereo,
// any sample rate (16k / 22.05k / 24k). Walks the chunk list to find "fmt " and
// "data" (tolerates a LIST/INFO chunk before data). No seeking, no resampling —
// the rate is handed to the codec via audio_out_set_rate().
// =============================================================================

#include <Arduino.h>
#include <stdint.h>

struct WavInfo {
    uint32_t sample_rate = 0;
    uint16_t channels    = 0;
    uint16_t bits        = 0;
    uint32_t data_len    = 0;   // bytes of PCM payload
    bool     valid       = false;
};

// Parse the header from a Stream positioned at byte 0. On success the stream is
// left positioned at the first PCM byte and `data_len` bytes remain to read.
bool wav_parse(Stream& in, WavInfo& out);

// Parse + stream the whole file to audio_out (sets the output rate first).
// `in` must be positioned at byte 0. Returns bytes of PCM pushed.
// `abort_flag`, if non-null and set true mid-play, stops early (barge-in / cue).
uint32_t wav_play(Stream& in, volatile bool* abort_flag = nullptr);
// Optional same-task abort check between chunks (true = stop now). Leave nullptr
// except around interruptible playback (tree asides).
extern bool (*g_wav_poll)();

// Same, from an in-RAM WAV image (used for the placeholder-audio bench path).
uint32_t wav_play_mem(const uint8_t* buf, size_t len, volatile bool* abort_flag = nullptr);

// Diagnostics (2026-09-16, field-test night): wav_play() increments these when
// a play ends with meaningfully fewer bytes pushed than the WAV's own data_len
// AND it wasn't a deliberate abort_flag stop — i.e. the SD/stream read came up
// short mid-file. Cheap, additive, read-only counters; not reset on read so a
// caller can diff two snapshots. Intended to be folded into an MQTT status
// payload so a short/corrupt-read pattern is visible without serial access.
extern volatile uint32_t g_wav_plays;
extern volatile uint32_t g_wav_short_plays;
extern volatile uint32_t g_wav_bytes_missing;

// Build a tiny valid WAV (sine tone) into a caller buffer. Returns total bytes
// written, or 0 if the buffer is too small. 16-bit mono.
size_t wav_make_tone(uint8_t* buf, size_t cap, uint32_t sample_rate,
                     uint32_t freq_hz, uint32_t ms, float amplitude);
