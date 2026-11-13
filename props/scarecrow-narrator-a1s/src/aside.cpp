#include "aside.h"
#include "fleet.h"
#include "mqtt_client.h"
#include "pools.h"
#include "wav.h"
#include <SD_MMC.h>

// Tunables (bench-adjust): a short beat after the tree's own action sound
// starts, how long a motion report stays relevant, and the minimum gap
// between asides so it stays a surprise, not a loop.
static const uint32_t ASIDE_DELAY_MS    = 1500;
static const uint32_t ASIDE_FRESH_MS    = 6000;
static const uint32_t ASIDE_COOLDOWN_MS = 90000;
static const uint8_t  MAX_FILES         = 64;
static const uint8_t  RECENT            = 8;       // don't repeat one of the last N lines

static AsideStats s_st = {};
static uint32_t   s_lastHandledMotion = 0;
static uint32_t   s_lastPlayMs = 0;
static bool       s_forced = false;
static uint8_t    s_recent[RECENT];
static uint8_t    s_recentN = 0;
static bool       s_pollFired = false;

static bool pollShowMessage() {                    // runs on the playing task
    if (mqtt_rx_pending()) { s_pollFired = true; return true; }
    return false;
}

void aside_begin() {
    char path[48];
    uint8_t n = 0;
    while (n < MAX_FILES) {
        snprintf(path, sizeof(path), "/scarecrow/aside/tree/%03u.wav", n);
        if (!SD_MMC.exists(path)) break;
        n++;
    }
    s_st.files = n;
    Serial.printf("[aside] %u tree-aside lines on the card%s\n", n,
                  n ? "" : " -- tree motion will be logged but not voiced");
}

static uint8_t pick() {
    for (int tries = 0; tries < 12; tries++) {
        uint8_t i = (uint8_t)(esp_random() % s_st.files);
        bool recent = false;
        for (uint8_t k = 0; k < s_recentN; k++) if (s_recent[k] == i) recent = true;
        if (!recent || s_st.files <= RECENT) return i;
    }
    return (uint8_t)(esp_random() % s_st.files);
}

static void remember(uint8_t i) {
    if (s_recentN < RECENT) s_recent[s_recentN++] = i;
    else { memmove(s_recent, s_recent + 1, RECENT - 1); s_recent[RECENT - 1] = i; }
}

static void play(bool paOn, void (*setPA)(bool)) {
    uint8_t i = pick();
    char path[48];
    snprintf(path, sizeof(path), "/scarecrow/aside/tree/%03u.wav", i);
    Serial.printf("[aside] tree motion -> line %u\n", i);
    if (!paOn && setPA) setPA(true);
    s_pollFired = false;
    g_wav_poll = pollShowMessage;
    int r = pools_play_file(path);
    g_wav_poll = nullptr;
    if (!paOn && setPA) setPA(false);             // leave the amp as we found it
    s_lastPlayMs = millis();
    remember(i);
    s_st.played++;
    if (s_pollFired) {
        s_st.aborted++;
        Serial.println(F("[aside] stopped early: show message arrived (show takes over)"));
    } else if (!r) {
        Serial.printf("[aside] line %u unreadable\n", i);
    }
}

void aside_force() { s_forced = true; }

void aside_tick(bool idle, bool paOn, void (*setPA)(bool)) {
    if (s_forced) {
        s_forced = false;
        if (!idle)          { Serial.println(F("[aside] bench: not idle, skipped")); return; }
        if (!s_st.files)    { Serial.println(F("[aside] bench: no lines on the card")); return; }
        play(paOn, setPA);
        return;
    }

    uint32_t m = fleet_tree_motion_ms();
    if (m == 0 || m == s_lastHandledMotion) return;
    uint32_t now = millis();
    if (now - m < ASIDE_DELAY_MS) return;         // let the tree's own sound lead
    s_lastHandledMotion = m;                      // each motion is handled once

    if (now - m > ASIDE_FRESH_MS) { s_st.skipped_stale++; return; }
    if (!idle) {
        s_st.skipped_busy++;
        Serial.println(F("[aside] tree motion while the show is busy -- ignored"));
        return;
    }
    if (!s_st.files) { s_st.skipped_nofiles++; return; }
    if (s_lastPlayMs && now - s_lastPlayMs < ASIDE_COOLDOWN_MS) { s_st.skipped_cooldown++; return; }
    play(paOn, setPA);
}

AsideStats aside_stats() { return s_st; }
