#include "middle.h"
#include "wav.h"
#include "audio_out.h"
#include <WiFi.h>
#include <HTTPClient.h>

// Flavor/outro lines are 1-4 sentences -- a few hundred KB at 24 kHz mono.
// Cap the buffer well above that; PSRAM has 4 MB.
static const size_t MIDDLE_MAX = 1024 * 1024;

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
