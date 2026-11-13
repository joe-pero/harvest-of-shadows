#include "middle.h"
#include "wav.h"
#include "audio_out.h"
#include <WiFi.h>
#include <HTTPClient.h>

// MIDDLE is 1-3 sentences — a few hundred KB at 24 kHz mono. Cap the buffer well
// above that; PSRAM has 4 MB.
static const size_t MIDDLE_MAX = 1024 * 1024;

MiddleResult middle_fetch_and_play(const char* base_url, uint32_t session,
                                   uint32_t deadline_ms, volatile bool* abort_flag) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[middle] no WiFi — skip to bridge"));
        return MID_NONE;
    }

    char url[128];
    snprintf(url, sizeof(url), "%s/middle/%08lx_sc.wav", base_url, (unsigned long)session);
    Serial.printf("[middle] polling %s (deadline %lu ms)\n", url, (unsigned long)deadline_ms);

    uint32_t start = millis();
    uint8_t* buf = nullptr;
    int      got = 0;
    int      contentLen = -1;
    bool     have = false;

    while (millis() - start < deadline_ms) {
        if (abort_flag && *abort_flag) { free(buf); return MID_NONE; }

        HTTPClient http;
        http.setConnectTimeout(2000);
        http.setTimeout(4000);
        if (!http.begin(url)) { delay(500); continue; }
        int code = http.GET();

        if (code == 200) {
            contentLen = http.getSize();
            size_t cap = (contentLen > 0 && (size_t)contentLen < MIDDLE_MAX) ? contentLen : MIDDLE_MAX;
            buf = (uint8_t*)ps_malloc(cap);
            if (!buf) { Serial.println(F("[middle] ps_malloc failed")); http.end(); return MID_NONE; }

            WiFiClient* st = http.getStreamPtr();
            uint32_t t0 = millis();
            while (http.connected() && (size_t)got < cap && millis() - t0 < 8000) {
                size_t avail = st->available();
                if (avail) {
                    int r = st->readBytes(buf + got, min(avail, cap - got));
                    got += r;
                    if (contentLen > 0 && got >= contentLen) break;
                } else delay(2);
            }
            http.end();
            have = (got > 44) && (contentLen < 0 || got >= contentLen);
            Serial.printf("[middle] downloaded %d / %d bytes  ok=%d\n", got, contentLen, have);
            break;
        }
        http.end();
        // 404 / 202 / etc = "not ready" — keep polling
        delay(500);
    }

    if (!have) {
        free(buf);
        if (millis() - start >= deadline_ms) {
            Serial.println(F("[middle] deadline — 1.5 s pad, then bridge"));
            audio_out_silence_ms(1500);
            return MID_PAD;
        }
        return MID_NONE;
    }

    // quick WAV sanity before committing a single sample
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        Serial.println(F("[middle] not a WAV — bridge"));
        free(buf);
        return MID_NONE;
    }

    Serial.println(F("[middle] playing live MIDDLE"));
    uint32_t pushed = wav_play_mem(buf, got, abort_flag);
    free(buf);
    return pushed > 0 ? MID_PLAYED : MID_NONE;
}

// =============================================================================
// Opportunistic single-check delivery — see middle.h. NOT YET BENCH-VERIFIED
// (written 2026-09-18 for the opportunistic-check redesign; flash + test
// before trusting live).
// =============================================================================

MiddleResult middle_check_and_play(const char* base_url, const char* suffix,
                                   uint32_t session, volatile bool* abort_flag) {
    if (WiFi.status() != WL_CONNECTED) return MID_NONE;
    if (abort_flag && *abort_flag) return MID_NONE;

    char url[144];
    snprintf(url, sizeof(url), "%s/middle/%08lx_%s.wav", base_url,
             (unsigned long)session, suffix);

    HTTPClient http;
    http.setConnectTimeout(1000);
    http.setTimeout(2000);
    if (!http.begin(url)) return MID_NONE;
    int code = http.GET();
    if (code != 200) {
        http.end();
        return MID_NONE;   // not ready yet -- caller tries again at the next checkpoint
    }

    int contentLen = http.getSize();
    size_t cap = (contentLen > 0 && (size_t)contentLen < MIDDLE_MAX) ? contentLen : MIDDLE_MAX;
    uint8_t* buf = (uint8_t*)ps_malloc(cap);
    if (!buf) {
        Serial.println(F("[middle] check_and_play: ps_malloc failed"));
        http.end();
        return MID_NONE;
    }

    WiFiClient* st = http.getStreamPtr();
    int got = 0;
    uint32_t t0 = millis();
    while (http.connected() && (size_t)got < cap && millis() - t0 < 8000) {
        if (abort_flag && *abort_flag) { free(buf); http.end(); return MID_NONE; }
        size_t avail = st->available();
        if (avail) {
            int r = st->readBytes(buf + got, min(avail, cap - got));
            got += r;
            if (contentLen > 0 && got >= contentLen) break;
        } else delay(2);
    }
    http.end();

    bool have = (got > 44) && (contentLen < 0 || got >= contentLen);
    Serial.printf("[middle] check_and_play(%s): downloaded %d / %d bytes  ok=%d\n",
                  suffix, got, contentLen, have);
    if (!have) { free(buf); return MID_NONE; }

    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        Serial.println(F("[middle] check_and_play: not a WAV"));
        free(buf);
        return MID_NONE;
    }

    Serial.printf("[middle] check_and_play(%s): playing\n", suffix);
    uint32_t pushed = wav_play_mem(buf, got, abort_flag);
    free(buf);
    return pushed > 0 ? MID_PLAYED : MID_NONE;
}

// =============================================================================
// Pipelined parts — NOT YET BENCH-VERIFIED, see middle.h. Shares the same
// poll/download shape as middle_fetch_and_play() above, factored out so
// tick_fn can be serviced during every wait instead of a bare delay().
// =============================================================================

// One file, one deadline. Returns true + fills *buf/*got on success. Same
// download logic as middle_fetch_and_play() above (kept in sync by hand --
// small enough that a shared helper wasn't worth the indirection tonight).
static bool _poll_and_download(const char* url, uint32_t deadline_ms,
                               void (*tick_fn)(), volatile bool* abort_flag,
                               uint8_t** out_buf, int* out_got) {
    uint32_t start = millis();
    uint8_t* buf = nullptr;
    int      got = 0;
    int      contentLen = -1;

    while (millis() - start < deadline_ms) {
        if (abort_flag && *abort_flag) { free(buf); return false; }
        if (tick_fn) tick_fn();

        HTTPClient http;
        http.setConnectTimeout(2000);
        http.setTimeout(4000);
        if (!http.begin(url)) { delay(500); continue; }
        int code = http.GET();

        if (code == 200) {
            contentLen = http.getSize();
            size_t cap = (contentLen > 0 && (size_t)contentLen < MIDDLE_MAX) ? contentLen : MIDDLE_MAX;
            buf = (uint8_t*)ps_malloc(cap);
            if (!buf) { Serial.println(F("[middle] ps_malloc failed")); http.end(); return false; }

            WiFiClient* st = http.getStreamPtr();
            uint32_t t0 = millis();
            while (http.connected() && (size_t)got < cap && millis() - t0 < 8000) {
                if (tick_fn) tick_fn();
                size_t avail = st->available();
                if (avail) {
                    int r = st->readBytes(buf + got, min(avail, cap - got));
                    got += r;
                    if (contentLen > 0 && got >= contentLen) break;
                } else delay(2);
            }
            http.end();
            bool have = (got > 44) && (contentLen < 0 || got >= contentLen);
            Serial.printf("[middle] downloaded %d / %d bytes  ok=%d\n", got, contentLen, have);
            if (!have) { free(buf); return false; }
            *out_buf = buf;
            *out_got = got;
            return true;
        }
        http.end();
        delay(500);   // 404 / 202 / etc = "not ready" -- keep polling
    }
    return false;
}

uint32_t middle_fetch_and_play_parts(const char* base_url, uint32_t session,
                                     uint32_t part1_deadline_ms,
                                     uint32_t next_part_deadline_ms,
                                     void (*tick_fn)(),
                                     volatile bool* abort_flag) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[middle] no WiFi — skip to bridge"));
        return 0;
    }

    uint32_t played = 0;
    for (uint32_t part = 1; ; part++) {
        if (abort_flag && *abort_flag) break;

        char url[144];
        snprintf(url, sizeof(url), "%s/middle/%08lx_sc_part%lu.wav", base_url,
                 (unsigned long)session, (unsigned long)part);
        uint32_t deadline = (part == 1) ? part1_deadline_ms : next_part_deadline_ms;
        Serial.printf("[middle] polling part %lu: %s (deadline %lu ms)\n",
                      (unsigned long)part, url, (unsigned long)deadline);

        uint8_t* buf = nullptr;
        int got = 0;
        if (!_poll_and_download(url, deadline, tick_fn, abort_flag, &buf, &got)) {
            Serial.printf("[middle] part %lu not available in time — stopping "
                          "(%lu part(s) played)\n", (unsigned long)part, (unsigned long)played);
            break;
        }
        Serial.printf("[middle] playing part %lu\n", (unsigned long)part);
        uint32_t pushed = wav_play_mem(buf, got, abort_flag);
        free(buf);
        if (pushed == 0) break;   // corrupt/empty part -- stop, don't guess at more
        played++;
    }
    return played;
}
