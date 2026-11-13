#include "banter.h"
#include "NarratorBanter.h"
#include "fleet.h"
#include "pools.h"
#include <SD_MMC.h>

// Margin on top of Burt's clip length when waiting for his DONE: cue delivery
// + his SD open + his DONE broadcast. Generous on purpose; a real miss still
// ends in seconds, not the old 30s blanket wait.
static const uint32_t BURT_TURN_MARGIN_MS = 4000;
static const uint8_t  MAX_LINES = 4;
static const char*    INDEX_PATH = "/scarecrow/banter/index.csv";

struct Line { uint8_t line; bool mine; uint32_t dur_ms; };

static BanterResult s_last = { -1, 0, 0, 0, "none" };

// Scan index.csv (pool,idx,line,speaker,duration_ms) for this exchange.
// Returns line count, 0 if the exchange isn't on the card, -1 if no index.
static int lookup(uint8_t pool, uint16_t idx, Line* out) {
    File f = SD_MMC.open(INDEX_PATH, FILE_READ);
    if (!f) return -1;
    const char* want = BANTER_POOL_NAMES[pool];
    int n = 0;
    char buf[96];
    while (f.available() && n < MAX_LINES) {
        size_t len = f.readBytesUntil('\n', buf, sizeof(buf) - 1);
        buf[len] = 0;
        char* save = nullptr;
        char* p_pool = strtok_r(buf, ",", &save);
        char* p_idx  = strtok_r(nullptr, ",", &save);
        char* p_line = strtok_r(nullptr, ",", &save);
        char* p_spk  = strtok_r(nullptr, ",", &save);
        char* p_dur  = strtok_r(nullptr, ",\r", &save);
        if (!p_pool || !p_idx || !p_line || !p_spk || !p_dur) continue;
        if (strcmp(p_pool, want) != 0 || (uint16_t)atoi(p_idx) != idx) continue;
        uint8_t ln = (uint8_t)atoi(p_line);
        if (ln < 1 || ln > MAX_LINES) continue;
        out[ln - 1] = { ln, strcmp(p_spk, "scarecrow") == 0, (uint32_t)atol(p_dur) };
        n = max<int>(n, ln);
    }
    f.close();
    for (int i = 0; i < n; i++)          // a gap in line numbers = a broken index row
        if (out[i].line != i + 1) return 0;
    return n;
}

static BanterResult finish(BanterResult r) {
    s_last = r;
    Serial.printf("[banter] %s #%u: %s (%u/%u lines heard)\n",
                  r.pool >= 0 ? BANTER_POOL_NAMES[r.pool] : "-", r.idx, r.outcome,
                  r.played, r.total);
    return r;
}

BanterResult banter_run(uint8_t pool, uint16_t idx, ShowTheme theme, ShowMode tone,
                        uint32_t session) {
    BanterResult r = { (int8_t)pool, idx, 0, 0, "complete" };
    if (pool >= BANTER_POOL_COUNT) { r.outcome = "not_on_card"; return finish(r); }

    Line lines[MAX_LINES] = {};
    int n = lookup(pool, idx, lines);
    if (n < 0) { r.outcome = "no_index";    return finish(r); }
    if (n == 0) { r.outcome = "not_on_card"; return finish(r); }
    r.total = (uint8_t)n;

    bool needsBurt = false;
    for (int i = 0; i < n; i++) if (!lines[i].mine) needsBurt = true;
    if (needsBurt && !fleet_probe_burt()) {
        r.outcome = "burt_unreachable";          // don't start: a setup line with no answer breaks the logic
        return finish(r);
    }

    Serial.printf("[banter] start %s #%u (%d lines)\n", BANTER_POOL_NAMES[pool], idx, n);
    for (int i = 0; i < n; i++) {
        const Line& L = lines[i];
        if (L.mine) {
            char path[64];
            snprintf(path, sizeof(path), "/scarecrow/banter/%s/%03u_%u.wav",
                     BANTER_POOL_NAMES[pool], idx, L.line);
            if (!pools_play_file(path)) { r.outcome = "sc_file_missing"; return finish(r); }
        } else {
            uint32_t turn = banter_turn((uint8_t)tone, pool, L.line, idx);
            fleet_arm_burt_turn(session, turn);
            fleet_cue_burt_turn(theme, turn);
            int w = fleet_wait_burt_turn(L.dur_ms + BURT_TURN_MARGIN_MS);
            if (w < 0) { r.outcome = "burt_timeout"; return finish(r); }
            if (w == 0) { r.outcome = "burt_failed";  return finish(r); }
        }
        r.played++;
    }
    return finish(r);
}

BanterResult banter_last() { return s_last; }
