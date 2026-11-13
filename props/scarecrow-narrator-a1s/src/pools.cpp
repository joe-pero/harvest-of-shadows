#include "pools.h"
#include "pins_a1s.h"
#include "wav.h"
#include "audio_out.h"
#include <SD_MMC.h>
#include <FS.h>

// ── bucket / segment naming ─────────────────────────────────────────────────
static const char* theme_str(ShowTheme t) {
    switch (t) {
        case THEME_CORN:            return "corn";
        case THEME_ROTLD:           return "rotld";
        case THEME_MUTANT_PUMPKINS: return "pumpkins";
        default:                    return "corn";
    }
}
static const char* tone_str(ShowMode m) {
    return (m == MODE_FRIENDLY) ? "friendly" : "scary";   // INTENSE -> scary (inert)
}
static const char* seg_str(PoolSegment s) {
    return s == SEG_INTRO ? "intro" : s == SEG_OUTRO ? "outro"
         : s == SEG_HOLD  ? "hold"  : "bridge";
}

void pools_bucket_name(ShowTheme theme, ShowMode tone, char* out, size_t cap) {
    snprintf(out, cap, "%s_%s", theme_str(theme), tone_str(tone));
}

// ── index.csv table ─────────────────────────────────────────────────────────
// intro[6] + outro[6] + hold[6] + bridge[1]. Bucket order for intro/outro/hold:
//   0 corn_scary 1 corn_friendly 2 rotld_scary 3 rotld_friendly
//   4 pumpkins_scary 5 pumpkins_friendly
static int      s_intro[6]  = {0};
static int      s_outro[6]  = {0};
static int      s_hold[6]   = {0};
static int      s_bridge    = 0;
static uint32_t s_version   = 0;
static bool     s_mounted   = false;

static int bucket_index(ShowTheme theme, ShowMode tone) {
    int t = (theme == THEME_CORN) ? 0 : (theme == THEME_ROTLD) ? 1 : 2;
    return t * 2 + (tone == MODE_FRIENDLY ? 1 : 0);
}

static void store_count(const char* seg, const char* bucket, int count) {
    if (strcmp(seg, "bridge") == 0) { s_bridge = count; return; }
    int* tbl = (strcmp(seg, "intro") == 0) ? s_intro
             : (strcmp(seg, "outro") == 0) ? s_outro
             : (strcmp(seg, "hold")  == 0) ? s_hold : nullptr;
    if (!tbl) return;
    static const char* names[6] = {
        "corn_scary", "corn_friendly", "rotld_scary", "rotld_friendly",
        "pumpkins_scary", "pumpkins_friendly"
    };
    for (int i = 0; i < 6; i++) if (strcmp(bucket, names[i]) == 0) { tbl[i] = count; return; }
}

static bool read_index() {
    File f = SD_MMC.open("/scarecrow/index.csv", FILE_READ);
    if (!f) { Serial.println(F("[pools] /scarecrow/index.csv not found")); return false; }
    int rows = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length() || line.startsWith("#")) continue;
        if (line.startsWith("pool_version")) {
            int c = line.indexOf(',');
            if (c > 0) s_version = (uint32_t)line.substring(c + 1).toInt();
            continue;
        }
        if (line.startsWith("segment,")) continue;                 // header
        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        if (c1 < 0 || c2 < 0) continue;
        String seg = line.substring(0, c1);
        String bkt = line.substring(c1 + 1, c2);
        int    cnt = line.substring(c2 + 1).toInt();
        store_count(seg.c_str(), bkt.c_str(), cnt);
        rows++;
    }
    f.close();
    Serial.printf("[pools] index.csv: %d rows, pool_version=%lu\n", rows, (unsigned long)s_version);
    Serial.printf("[pools]   intro  cs=%d cf=%d rs=%d rf=%d ps=%d pf=%d\n",
                  s_intro[0], s_intro[1], s_intro[2], s_intro[3], s_intro[4], s_intro[5]);
    Serial.printf("[pools]   outro  cs=%d cf=%d rs=%d rf=%d ps=%d pf=%d\n",
                  s_outro[0], s_outro[1], s_outro[2], s_outro[3], s_outro[4], s_outro[5]);
    Serial.printf("[pools]   hold   cs=%d cf=%d rs=%d rf=%d ps=%d pf=%d\n",
                  s_hold[0], s_hold[1], s_hold[2], s_hold[3], s_hold[4], s_hold[5]);
    Serial.printf("[pools]   bridge/generic=%d\n", s_bridge);
    return rows > 0;
}

bool pools_begin() {
    SD_MMC.end();
    if (!SD_MMC.setPins(A1S_SD_CLK, A1S_SD_CMD, A1S_SD_D0)) {
        Serial.println(F("[pools] SD_MMC.setPins rejected"));
        return false;
    }
    // 1-bit, no auto-format, conservative clock (raise later once proven).
    // Retry a few times — this only helps a plain SDMMC init race on power-up.
    // It does NOT and CANNOT help the known GPIO12/VDD_SDIO flash-voltage-strap
    // conflict seen on Burt (intermittently — see skeleton-narrator-a1s's
    // BUILD_LOG.md 2026-09-09/11): that failure happens in the boot ROM before
    // any of this code ever runs, so a board hit by it never gets here at all.
    // Scarecrow hasn't shown that fault, but kept in sync with Burt's pools.cpp
    // per this project's own "don't let the two nodes drift" rule.
    const uint8_t kSdMountAttempts = 3;
    bool sdOk = false;
    for (uint8_t i = 1; i <= kSdMountAttempts && !sdOk; i++) {
        sdOk = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5);
        if (!sdOk && i < kSdMountAttempts) {
            Serial.printf("[pools] SD mount attempt %u/%u failed — retrying\n", i, kSdMountAttempts);
            SD_MMC.end();
            delay(300);
        }
    }
    if (!sdOk) {
        Serial.printf("[pools] SD mount failed after %u attempts (no card? DIP? pull-ups?) — running without pools\n",
                      kSdMountAttempts);
        s_mounted = false;
        return false;
    }
    Serial.printf("[pools] SD mounted: %.2f GB, type %d\n",
                  SD_MMC.cardSize() / (1024.0 * 1024.0 * 1024.0), SD_MMC.cardType());
    s_mounted = read_index();
    return s_mounted;
}

bool     pools_mounted() { return s_mounted; }
uint32_t pools_version() { return s_version; }

int pools_count(PoolSegment seg, ShowTheme theme, ShowMode tone) {
    if (seg == SEG_BRIDGE) return s_bridge;
    const int* tbl = (seg == SEG_INTRO) ? s_intro
                   : (seg == SEG_OUTRO) ? s_outro : s_hold;   // SEG_HOLD
    return tbl[bucket_index(theme, tone)];
}

// ── anti-repeat: last 4 indices per (segment,bucket) ─────────────────────────
// first dim = PoolSegment count (INTRO/OUTRO/HOLD/BRIDGE = 4)
static uint8_t s_recent[4][6][4];
static uint8_t s_recentPos[4][6] = {{0}};

static bool was_recent(int seg, int bkt, uint8_t idx) {
    for (int i = 0; i < 4; i++) if (s_recent[seg][bkt][i] == idx + 1) return true;
    return false;
}
static void mark_recent(int seg, int bkt, uint8_t idx) {
    s_recent[seg][bkt][s_recentPos[seg][bkt]] = idx + 1;
    s_recentPos[seg][bkt] = (s_recentPos[seg][bkt] + 1) & 3;
}

static int play_path(const char* path, volatile bool* abort_flag) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return 0;
    Serial.printf("[pools] play %s\n", path);
    uint32_t n = wav_play(f, abort_flag);
    f.close();
    return n > 0 ? 1 : 0;
}

// 2026-09-24 OUTRO banter: play one exact file (the exchange's line). 1 = played,
// 0 = not mounted / missing / unreadable -- caller logs and stops the exchange.
int pools_play_file(const char* path, volatile bool* abort_flag) {
    if (!s_mounted) return 0;
    int r = play_path(path, abort_flag);
    if (!r) Serial.printf("[pools] MISSING/unreadable: %s\n", path);
    return r;
}

// 2026-09-22: matched-pair OUTRO delivery (Joe's "go back and forth, fill
// each other out" design for the static fallback pool -- see
// docs/show-trigger-geometry.md and PROJECT_LOG.md 2026-09-22). The new
// outro/<bucket>/NNN.wav pairs are WRITTEN to interlock at the same index
// across Scarecrow's and Burt's cards (line 017 always pairs with line
// 017), so both boards must land on the identical index for a given visit.
// Neither board knows the other's random draw and there's no new ESP-NOW
// message for this -- instead both derive the SAME index from the one
// thing they already both have: `session` (the retained harvest/show/params
// session_id, latched identically on both boards via MQTT / the H1b
// handoff). Deliberately skips the anti-repeat re-roll `pools_play()` uses
// -- re-rolling here would desync the two boards for that visit; a little
// index repetition across visits is an acceptable tradeoff for a guaranteed
// pair. KNOWN EDGE CASE, not fixed: if the two SD cards' file sets differ at
// the chosen index (one corrupt/missing, the other fine), the walk-forward
// fallback below can still shift the two boards apart by a step or two --
// same risk profile as any two independently-provisioned SD cards, not
// something this function can fully close. NOT YET BENCH-VERIFIED.
int pools_play_matched(PoolSegment seg, ShowTheme theme, ShowMode tone,
                        uint32_t session, volatile bool* abort_flag) {
    if (!s_mounted) return 0;

    char path[64];
    char bkt[20];
    pools_bucket_name(theme, tone, bkt, sizeof(bkt));
    int bi = bucket_index(theme, tone);
    int n  = pools_count(seg, theme, tone);
    if (n > 0) {
        uint8_t idx = (uint8_t)(session % (uint32_t)n);
        for (int step = 0; step < 4; step++) {             // bad-sector walk-forward
            snprintf(path, sizeof(path), "/scarecrow/%s/%s/%03u.wav",
                     seg_str(seg), bkt, (idx + step) % n);
            if (play_path(path, abort_flag) == 1) {
                mark_recent(seg, bi, (idx + step) % n);
                return 2;
            }
        }
        Serial.printf("[pools] matched %s/%s idx=%u unavailable -> bridge\n",
                       seg_str(seg), bkt, idx);
    }

    // bridge / generic fallback -- NOT matched (no shared bucket to align on),
    // same as pools_play()'s own fallback. A visit that falls this far back
    // loses the interlock; still better than dead air.
    if (s_bridge > 0) {
        uint8_t idx = esp_random() % s_bridge;
        for (int step = 0; step < 4; step++) {
            snprintf(path, sizeof(path), "/scarecrow/bridge/generic/%03u.wav",
                     (idx + step) % s_bridge);
            if (play_path(path, abort_flag) == 1) {
                mark_recent(SEG_BRIDGE, 0, (idx + step) % s_bridge);
                return 1;
            }
        }
    }
    return 0;
}

int pools_play(PoolSegment seg, ShowTheme theme, ShowMode tone, volatile bool* abort_flag) {
    if (!s_mounted) return 0;

    char path[64];
    if (seg != SEG_BRIDGE) {
        char bkt[20];
        pools_bucket_name(theme, tone, bkt, sizeof(bkt));
        int bi = bucket_index(theme, tone);
        int n  = pools_count(seg, theme, tone);
        if (n > 0) {
            uint8_t idx = esp_random() % n;
            for (int tries = 0; tries < 6; tries++) {          // anti-repeat re-roll
                if (!was_recent(seg, bi, idx)) break;
                idx = esp_random() % n;
            }
            for (int step = 0; step < 4; step++) {             // bad-sector walk-forward
                snprintf(path, sizeof(path), "/scarecrow/%s/%s/%03u.wav",
                         seg_str(seg), bkt, (idx + step) % n);
                if (play_path(path, abort_flag) == 1) {
                    mark_recent(seg, bi, (idx + step) % n);
                    return 2;
                }
            }
        }
        Serial.printf("[pools] %s/%s unavailable -> bridge\n", seg_str(seg), bkt);
    }

    // bridge / generic fallback (flat pool, Scary theme-agnostic)
    if (s_bridge > 0) {
        uint8_t idx = esp_random() % s_bridge;
        for (int step = 0; step < 4; step++) {
            snprintf(path, sizeof(path), "/scarecrow/bridge/generic/%03u.wav",
                     (idx + step) % s_bridge);
            if (play_path(path, abort_flag) == 1) {
                mark_recent(SEG_BRIDGE, 0, (idx + step) % s_bridge);
                return 1;
            }
        }
    }
    Serial.println(F("[pools] bridge unreadable too -> caller pads silence"));
    return 0;
}
