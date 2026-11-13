#include "wav.h"
#include "audio_out.h"

volatile uint32_t g_wav_plays         = 0;
volatile uint32_t g_wav_short_plays   = 0;
volatile uint32_t g_wav_bytes_missing = 0;

// ── tiny in-RAM Stream so wav_play() works for both File and memory sources ───
class MemStream : public Stream {
public:
    MemStream(const uint8_t* p, size_t n) : _p(p), _n(n), _i(0) {}
    int available() override      { return (int)(_n - _i); }
    int read() override           { return _i < _n ? _p[_i++] : -1; }
    int peek() override           { return _i < _n ? _p[_i] : -1; }
    size_t readBytes(char* dst, size_t len) {
        size_t k = _n - _i; if (k > len) k = len;
        memcpy(dst, _p + _i, k); _i += k; return k;
    }
    size_t write(uint8_t) override { return 0; }
private:
    const uint8_t* _p; size_t _n; size_t _i;
};

static uint32_t rd_u32(Stream& s) {
    uint8_t b[4]; s.readBytes((char*)b, 4);
    return (uint32_t)b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint16_t rd_u16(Stream& s) {
    uint8_t b[2]; s.readBytes((char*)b, 2);
    return (uint16_t)(b[0] | (b[1] << 8));
}
static void skip(Stream& s, uint32_t n) {
    uint8_t junk[64];
    while (n) { size_t k = n > sizeof(junk) ? sizeof(junk) : n; s.readBytes((char*)junk, k); n -= k; }
}

bool wav_parse(Stream& in, WavInfo& out) {
    char id[4];
    if (in.readBytes(id, 4) != 4 || memcmp(id, "RIFF", 4) != 0) return false;
    (void)rd_u32(in);                                   // RIFF size
    if (in.readBytes(id, 4) != 4 || memcmp(id, "WAVE", 4) != 0) return false;

    bool have_fmt = false;
    for (int guard = 0; guard < 16; guard++) {
        if (in.readBytes(id, 4) != 4) return false;
        uint32_t csize = rd_u32(in);
        if (memcmp(id, "fmt ", 4) == 0) {
            uint16_t fmt   = rd_u16(in);
            out.channels   = rd_u16(in);
            out.sample_rate = rd_u32(in);
            (void)rd_u32(in);                            // byte rate
            (void)rd_u16(in);                            // block align
            out.bits       = rd_u16(in);
            if (csize > 16) skip(in, csize - 16);
            if (fmt != 1) return false;                  // PCM only
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            out.data_len = csize;
            out.valid    = have_fmt && out.bits == 16 &&
                           (out.channels == 1 || out.channels == 2) &&
                           out.sample_rate >= 8000 && out.sample_rate <= 48000;
            return out.valid;
        } else {
            skip(in, csize + (csize & 1));               // chunks are word-aligned
        }
    }
    return false;
}

// Optional abort check, called on THIS (playing) task between chunks. Set only
// around interruptible playback (tree asides, 2026-09-24); nullptr otherwise.
bool (*g_wav_poll)() = nullptr;

uint32_t wav_play(Stream& in, volatile bool* abort_flag) {
    WavInfo w;
    if (!wav_parse(in, w)) {
        Serial.println(F("[wav] parse failed (not PCM16 mono/stereo 8-48k)"));
        return 0;
    }
    Serial.printf("[wav] %lu Hz, %u ch, 16-bit, %lu bytes PCM (~%lu ms)\n",
                  (unsigned long)w.sample_rate, w.channels, (unsigned long)w.data_len,
                  (unsigned long)(w.data_len * 1000UL /
                                  (w.sample_rate * w.channels * 2UL)));
    audio_out_set_rate(w.sample_rate);

    static int16_t buf[1024];                            // 512 stereo frames / 1024 mono samples
    uint32_t remaining = w.data_len;
    uint32_t pushed    = 0;
    bool     aborted   = false;
    while (remaining >= 2) {
        if (abort_flag && *abort_flag) { Serial.println(F("[wav] aborted")); aborted = true; break; }
        if (g_wav_poll && g_wav_poll()) { Serial.println(F("[wav] aborted (poll)")); aborted = true; break; }
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        want &= ~((size_t)1);                            // whole samples
        size_t got = in.readBytes((char*)buf, want);
        if (got < 2) break;
        size_t samples = got / 2;
        if (w.channels == 1) audio_out_write_mono(buf, samples);
        else                 audio_out_write(buf, samples / 2);
        pushed    += got;
        remaining -= got;
    }
    g_wav_plays++;
    if (!aborted && pushed + 4 < w.data_len) {
        g_wav_short_plays++;
        g_wav_bytes_missing += (w.data_len - pushed);
        Serial.printf("[wav] SHORT: pushed %lu/%lu bytes (missing %lu)\n",
                      (unsigned long)pushed, (unsigned long)w.data_len,
                      (unsigned long)(w.data_len - pushed));
    }
    return pushed;
}

uint32_t wav_play_mem(const uint8_t* b, size_t len, volatile bool* abort_flag) {
    MemStream ms(b, len);
    return wav_play(ms, abort_flag);
}

size_t wav_make_tone(uint8_t* buf, size_t cap, uint32_t sr, uint32_t freq, uint32_t ms, float amp) {
    uint32_t nsamp = (uint64_t)sr * ms / 1000;
    size_t   need  = 44 + nsamp * 2;
    if (cap < need) return 0;
    if (amp < 0) amp = 0; if (amp > 1) amp = 1;

    auto put32 = [&](size_t o, uint32_t v) { buf[o]=v; buf[o+1]=v>>8; buf[o+2]=v>>16; buf[o+3]=v>>24; };
    auto put16 = [&](size_t o, uint16_t v) { buf[o]=v; buf[o+1]=v>>8; };

    memcpy(buf, "RIFF", 4);            put32(4, 36 + nsamp * 2);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);       put32(16, 16);
    put16(20, 1);                      put16(22, 1);            // PCM, mono
    put32(24, sr);                     put32(28, sr * 2);
    put16(32, 2);                      put16(34, 16);
    memcpy(buf + 36, "data", 4);       put32(40, nsamp * 2);

    const float w = 2.0f * (float)M_PI * (float)freq / (float)sr;
    for (uint32_t i = 0; i < nsamp; i++) {
        int16_t s = (int16_t)(amp * 30000.0f * sinf(w * i));
        buf[44 + i * 2]     = (uint8_t)s;
        buf[44 + i * 2 + 1] = (uint8_t)(s >> 8);
    }
    return need;
}
