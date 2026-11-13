// =============================================================================
// Scarecrow Narrator A1S — Steps 4+5: WiFi + ESP-NOW + full integration cycle
// =============================================================================
// Structural end-to-end firmware, validated with placeholder audio (no microSD
// card / no live vision pipeline yet — both drop in with zero firmware change).
//
// Per-cycle flow (docs/a1s-board-and-audio-architecture.md §3.2, H2 ordering):
//   1. fleet THEME burst   (SET_THEME + SET_MODE + SHOW_STATE, 3x)   -- radio first
//   2. INTRO from SD pool   (placeholder WAV until a card is provisioned)
//   3. MIDDLE: HTTP push-then-play from the bridge  (placeholder tone if absent)
//   4. OUTRO from SD pool
//   5. NARRATOR_1_DONE broadcast + H1b unicast cue to Burt (<=3 tries, non-block)
//   6. SHOW_STATE(ACTIVE) -> (creepy-theme-music phase runs on the fleet)
//
// Serial:  t = narrator self-test (INTRO->MIDDLE-standin->OUTRO, NO radio/Burt)
//          T = self-test with a HOLD line (INTRO->HOLD->OUTRO)
//          n = next (theme,tone) bucket        c = run one FULL cycle
//          a = toggle auto-cycle (30 s, for the 2-board handshake test)
//          x = coexistence test     X = coexistence test WITH playback
//          b = cue Burt now         h = handshake rollup     w = wifi/net status
//          d = pool table           + / - = volume           p = on-board amp
//
// FIRST-TEST NOTE (2026-09-08): auto-cycle now defaults OFF. On a fresh boot the
// board is quiet — press `t` for a clean Scarecrow-only audio check, `n` to step
// themes. Press `a` once Burt is connected for the continuous 2-board cycle.
//
// REAL CUE TRANSPORT (2026-09-11): the front-yard trigger is now MQTT, not
// serial — harvest/show/params (retained: session_id/theme/tone/cycle) latches
// the show, harvest/show/cue phase preshow|hold|mainshow|reset drives it
// (docs/a1s-board-and-audio-architecture.md §3.2). Every serial command above
// is untouched and still works independently for bench diagnostics.
//
// OPPORTUNISTIC MIDDLE/OUTRO (2026-09-18, NOT YET BENCH-VERIFIED): the real
// MQTT-driven doPreshow()/doHold()/doMainshow() below no longer BLOCK waiting
// on MIDDLE (the old `middle_fetch_and_play()` 4s-poll — still used by the
// bench-only runCycle()/'c'/'C', untouched). Each does one instant, non-
// blocking middle_check_and_play() at its top instead: ready -> play it as a
// bonus, not ready -> fall straight through, no filler line. MAIN-SHOW's
// OUTRO position also gets an AI-generated "outro" payoff line the SAME way —
// it's now the PRIMARY walkway content, with the prerecorded OUTRO pool as
// the fail-safe for a broken/late/abandoned AI return, not always-both like
// MIDDLE's bonus. See docs/_history/TASK-a1s-middle-parts-wiring.md, "THE ACTUAL PLAN
// (2026-09-18 revision)" and "MAIN-SHOW AI-outro" for the full design.
//
// 2026-09-25 (#5): PRE-SHOW now WAITS up to 30s after the INTRO for MIDDLE
// (waitForMiddle(); the bridge builds it during the INTRO). MQTT is pumped
// during that wait and any cue that arrives is queued, not re-entered.
// =============================================================================

#include <Arduino.h>
#include "pins_a1s.h"
#include "es8388.h"
#include "audio_out.h"
#include "wav.h"
#include "pools.h"
#include "fleet.h"
#include "middle.h"
#include "ota.h"
#include "mqtt_client.h"
#include "banter.h"
#include "aside.h"
#include "NarratorBanter.h"
#include "ProjectConstants.h"
#include "wifi_credentials.h"
#include "esp_system.h"

static const uint32_t BASE_RATE = 22050;
#define BRIDGE_URL "http://192.168.1.x:8080"     // audio_bridge.py :8080
static uint8_t  s_volume    = 98;   // 2026-09-28 evening: Joe, 98 (matched to Burt by ear on the live walk); was 59 earlier that day, 99 (09-17), 65 before
static bool     s_paOn      = false;
static bool     s_autoCycle = false;   // OFF by default (2026-09-08) — a fresh
                                       // boot is quiet for a controlled first
                                       // test. `a` enables it for the 2-board
                                       // handshake run once Burt is connected.
static const uint32_t AUTO_CYCLE_MS = 30000;
static const uint32_t FIRST_CYCLE_MS = 12000;   // let both boards settle first

// Rule 4 dead-man's switch (was 120000 / 2 min, a bench guess never checked
// against real latency). 2026-09-22: real orchestration/livebridge logs from
// the actual OpenAI+live-XTTS pipeline (photo fetch + OpenAI call + live
// voice synthesis of both narrator lines) measured 29-243s across 8
// successful runs, median 101.6s -- the old 120s deadline, combined with the
// ~40-47s INTRO + 40s HOLD_INTERVAL_S cadence, meant only the PRESHOW-start
// check (~t=intro_len) and ONE hold-tick check (~t=intro_len+40) ever
// actually ran before the deadline cut them off -- the 2nd and 3rd HOLD
// ticks (~t=intro_len+80, +120) were mathematically unreachable. Raised to
// 200s so all 3 HOLD_MAX_LOOPS ticks get their real shot (matches
// showsim/constants.py's HOLD_INTERVAL_S=40 x HOLD_MAX_LOOPS=3 cadence, plus
// the ~40-47s INTRO length, plus margin). Still won't catch the ~200-243s
// tail seen in real data -- that's an accepted miss, not a bug; a fixed
// deadline chasing the absolute worst case would mean cycles that never
// reach MAIN-SHOW just sit waiting instead of resetting. NOT YET
// BENCH-VERIFIED against real hardware -- see docs/TASK-a1s-middle-parts-
// wiring.md and PROJECT_LOG.md 2026-09-22.
static const uint32_t AI_DEADLINE_MS = 200000UL;

static uint8_t* s_phIntro = nullptr;  static size_t s_phIntroLen = 0;
static uint8_t* s_phOutro = nullptr;  static size_t s_phOutroLen = 0;

static uint32_t  s_session;
static ShowTheme s_theme = THEME_CORN;
static ShowMode  s_tone  = MODE_SCARY;

static bool     s_haveParams      = false;  // latched from a real harvest/show/params
static bool     s_showActive      = false;
static uint32_t s_showActiveUntil = 0;

// Opportunistic MIDDLE/AI-outro delivery state (2026-09-18, docs/TASK-a1s-
// middle-parts-wiring.md "THE ACTUAL PLAN"). Reset per-session in
// onShowParams(). s_aiDeadline is the shared Rule-4 dead-man's switch (2 min
// from PRESHOW start) for BOTH files -- past it, doPreshow/doHold/doMainshow
// stop checking the network for this session entirely.
static bool     s_middleDelivered = false;
// OUTRO banter for this session (from params; -1 = none -> OUTRO pool)
static int8_t   s_banterPool = -1;
static uint16_t s_banterIdx  = 0;
static bool     s_encore     = false;
static uint32_t s_lastCueMs  = 0;       // millis() of the last MQTT cue (tree-aside quiet time)
// Tree asides only when truly idle, and not right on the heels of a show.
static const uint32_t ASIDE_QUIET_AFTER_CUE_MS = 30000;
static uint32_t s_aiDeadline      = 0;

// Bounded MIDDLE wait after the INTRO (CURRENT-BUILD #5, Joe 2026-09-24): the
// bridge now assembles MIDDLE in the background DURING the INTRO, so after the
// INTRO Scarecrow waits for it and plays it the moment it lands. Max 30s, then
// fall through to the HOLD / MAIN-SHOW opportunistic checks as before.
static const uint32_t MIDDLE_WAIT_MS      = 30000;
static const uint32_t MIDDLE_POLL_MS      = 1000;

// Cues that arrive while a cue handler is running (only possible during the
// MIDDLE wait, the one place MQTT is pumped mid-cue) are queued and run in
// order afterwards, never re-entered. Also keeps the MQTT keepalive alive
// through the wait (PubSubClient default 15s).
static bool     s_inCue = false;
static ShowCue  s_cueQ[4];
static uint8_t  s_cueQHead = 0, s_cueQLen = 0;

static void setPA(bool on) {
    s_paOn = on;
    digitalWrite(A1S_PA_EN, on ? HIGH : LOW);
    Serial.printf("[main] on-board amp (GPIO21) = %s\n", on ? "HIGH" : "LOW");
}

static void advanceCue() {
    static uint8_t k = 0;
    k = (k + 1) % 6;
    s_theme   = (ShowTheme)(k / 2);
    s_tone    = (k & 1) ? MODE_FRIENDLY : MODE_SCARY;
    s_session = esp_random();
    fleet_set_session(s_session);
}

static const char* segLabel(PoolSegment s) {
    return s == SEG_INTRO ? "INTRO" : s == SEG_OUTRO ? "OUTRO"
         : s == SEG_HOLD  ? "HOLD"  : "BRIDGE";
}

// 2026-09-22: same shape as playPoolOr() below, but for the matched-pair
// OUTRO (pools_play_matched(), see pools.cpp) -- session-derived index so
// Burt's board lands on the same line. Only used at the real doMainshow()
// fallback call site, not the bench self-test paths.
static void playOutroMatchedOr(uint8_t* ph, size_t phLen) {
    int r = pools_play_matched(SEG_OUTRO, s_theme, s_tone, s_session, nullptr);
    if      (r == 2) Serial.println(F("[cycle] outro: matched pool line"));
    else if (r == 1) Serial.println(F("[cycle] outro: bridge fallback (unmatched)"));
    else {
        Serial.println(F("[cycle] outro: no SD pool -> placeholder WAV"));
        if (ph) wav_play_mem(ph, phLen, nullptr);
    }
}

static void playPoolOr(PoolSegment seg, uint8_t* ph, size_t phLen) {
    const char* label = segLabel(seg);
    int r = pools_play(seg, s_theme, s_tone, nullptr);
    if      (r == 2) Serial.printf("[cycle] %s: pool line\n", label);
    else if (r == 1) Serial.printf("[cycle] %s: bridge fallback\n", label);
    else {
        Serial.printf("[cycle] %s: no SD pool -> placeholder WAV\n", label);
        if (ph) wav_play_mem(ph, phLen, nullptr);
        else    audio_out_silence_ms(700);
    }
}

static void runCycle() {
    char bkt[20];
    pools_bucket_name(s_theme, s_tone, bkt, sizeof(bkt));
    Serial.printf("\n[cycle] ===== session 0x%08lX  bucket=%s =====\n",
                  (unsigned long)s_session, bkt);

    // 1. radio first (H2): the one cue the fleet depends on, off the air before
    //    the sustained HTTP transfer starts
    fleet_broadcast_theme_burst(s_theme, s_tone);

    // 2. INTRO — local, zero-latency
    playPoolOr(SEG_INTRO, s_phIntro, s_phIntroLen);

    // 3. MIDDLE — push-then-play from the bridge; placeholder if not reachable.
    //    Deadline models "INTRO_len - 1s" — use a short bench value here.
    MiddleResult mid = middle_fetch_and_play(BRIDGE_URL, s_session, 4000, nullptr);
    if (mid == MID_NONE) {
        Serial.println(F("[cycle] MIDDLE: unavailable -> bridge/generic pool line"));
        if (pools_play(SEG_BRIDGE, s_theme, s_tone, nullptr) == 0) {
            audio_out_tone(330, 220, 0.35f);   // pool empty too -> last-resort tone
            audio_out_silence_ms(900);
        }
    }

    // 4. OUTRO
    playPoolOr(SEG_OUTRO, s_phOutro, s_phOutroLen);

    // 5. narrator handoff — broadcast done + H1b unicast cue to Burt
    fleet_broadcast_narrator_done(1);
    FleetHandshake hs = fleet_cue_burt(s_theme, s_tone);
    (void)hs;
    // (Burt plays his line here; he unicasts CMD_ACK back — logged by fleet.cpp.
    //  When Burt signals done he broadcasts NARRATOR_2_DONE.)

    // 6. post-Burt: creepy-theme-music phase starts on the fleet
    fleet_broadcast_show_state(SHOW_ACTIVE);
    audio_out_flush();
    Serial.println(F("[cycle] ===== done =====\n"));
}

// Local narrator self-test — NO ESP-NOW burst, NO Burt cue, NO MIDDLE HTTP.
// Just "does the Scarecrow sound right out of the speaker" for the current
// (theme, tone).  t = INTRO -> bridge/generic stand-in -> OUTRO
//                 T = INTRO -> HOLD line -> OUTRO   (proves the hold/ pool)
static void narratorSelfTest(bool withHold) {
    char bkt[20];
    pools_bucket_name(s_theme, s_tone, bkt, sizeof(bkt));
    Serial.printf("\n[selftest] ===== %s  bucket=%s  (no radio / no Burt) =====\n",
                  withHold ? "INTRO->HOLD->OUTRO" : "INTRO->MIDDLE-standin->OUTRO", bkt);
    if (!s_paOn) setPA(true);

    playPoolOr(SEG_INTRO, s_phIntro, s_phIntroLen);

    if (withHold) {
        playPoolOr(SEG_HOLD, nullptr, 0);
    } else {
        Serial.println(F("[selftest] MIDDLE stand-in -> bridge/generic pool line"));
        if (pools_play(SEG_BRIDGE, s_theme, s_tone, nullptr) == 0) audio_out_silence_ms(500);
    }

    playPoolOr(SEG_OUTRO, s_phOutro, s_phOutroLen);
    audio_out_flush();
    Serial.println(F("[selftest] ===== done =====\n"));
}

static void netStatus() {
    Serial.printf("[net] wifi_up=%d  channel=%d (want %d)  ip=%s\n",
                  fleet_wifi_up(), fleet_wifi_channel(), WIFI_CHANNEL, fleet_ip());
    Serial.printf("[net] heap=%u  psram_free=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    Serial.printf("[net] mqtt_connected=%d\n", mqtt_connected());
}

// 2026-09-16 field-test night: folded in wav_play() short-read diagnostics
// (g_wav_* in wav.cpp/.h) + esp_reset_reason() so a garbled/cutout-audio
// symptom can be told apart from an SD read fault vs. a brownout/watchdog
// reset vs. neither, over MQTT, with no serial/USB access to the board.
// reset_reason is cheap to include on every publish (not just at boot) so a
// reset that happens between two 30s heartbeats still shows up promptly.
static void publishStatus() {
    FleetHandshakeStats st = fleet_handshake_stats();
    BanterResult bl = banter_last();
    AsideStats as = aside_stats();
    char json[600];
    snprintf(json, sizeof(json),
        "{\"status\":\"online\",\"sd\":%s,\"pool_version\":%lu,\"burt_sd\":\"%s\",\"show_active\":%s,\"cue_ok\":%lu,\"cue_fail\":%lu,\"burt_acks\":%lu,"
        "\"reset_reason\":%d,\"wav_plays\":%lu,\"wav_short\":%lu,\"wav_missing_b\":%lu,"
        "\"heap\":%u,\"psram\":%u,"
        "\"banter\":\"%s#%u\",\"banter_outcome\":\"%s\",\"banter_heard\":%u,\"banter_total\":%u,"
        "\"tree_motion_rx\":%lu,\"aside_played\":%lu,\"aside_aborted\":%lu,\"aside_busy\":%lu}",
        pools_mounted() ? "true" : "false", (unsigned long)pools_version(),
        fleet_burt_sd() == 1 ? "ok" : fleet_burt_sd() == 0 ? "FAILED" : "unknown",
        s_showActive ? "true" : "false",
        (unsigned long)st.cueOk, (unsigned long)st.cueFail, (unsigned long)st.burtAckRx,
        (int)esp_reset_reason(),
        (unsigned long)g_wav_plays, (unsigned long)g_wav_short_plays, (unsigned long)g_wav_bytes_missing,
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
        bl.pool >= 0 ? BANTER_POOL_NAMES[bl.pool] : "none", bl.idx, bl.outcome,
        bl.played, bl.total,
        (unsigned long)fleet_tree_motion_count(), (unsigned long)as.played,
        (unsigned long)as.aborted, (unsigned long)as.skipped_busy);
    mqtt_publish_status(json);
}

// harvest/show/params is retained, so this also fires on MQTT reconnect —
// that's the reboot-mid-cycle resync from the architecture doc, for free.
static void onShowParams(const ShowParams& p) {
    s_session = p.session_id;
    s_theme   = (ShowTheme)p.theme;
    s_tone    = (ShowMode)p.tone;
    fleet_set_session(s_session);
    s_haveParams = true;
    s_middleDelivered = false;
    s_banterPool = p.banter_pool;
    s_banterIdx  = p.banter_idx;
    s_encore     = p.encore;
    s_aiDeadline      = millis() + AI_DEADLINE_MS;   // Rule 4: dead-man's switch, from now
    // 2026-09-25: bridge says no MIDDLE is coming for this session -> expire the
    // deadline now, so the post-INTRO wait and the HOLD/MAIN-SHOW re-checks are
    // skipped instead of 30s of dead air (seen live, AI gated off).
    if (p.middle == 0) s_aiDeadline = millis();
    Serial.printf("[mqtt] params: session=0x%08lX theme=%u tone=%u cycle=%lu banter=%s#%u (%u lines)%s\n",
                  (unsigned long)p.session_id, p.theme, p.tone, (unsigned long)p.cycle,
                  p.banter_pool >= 0 ? BANTER_POOL_NAMES[p.banter_pool] : "none",
                  p.banter_idx, p.banter_lines, p.encore ? " ENCORE" : "");
    Serial.printf("[mqtt] params: middle=%s\n",
                  p.middle == 1 ? "expected" : p.middle == 0 ? "NONE (wait skipped)" : "unknown (old bridge)");
}

static bool cueQueued(const char* phase) {
    for (uint8_t i = 0; i < s_cueQLen; i++)
        if (!strcmp(s_cueQ[(s_cueQHead + i) % 4].phase, phase)) return true;
    return false;
}

// Poll for this session's MIDDLE after the INTRO. Keeps MQTT/WiFi serviced
// (incoming cues are queued, see onShowCue). Stops early when the group has
// moved on: a queued mainshow / reset / new preshow, or new params. A queued
// HOLD does NOT stop it -- MIDDLE beats a taunt; the HOLD runs right after.
// 2026-09-28 (Joe: "I don't want Burt talking over the Scarecrow"): after
// cueing Burt, hold the floor until his NARRATOR_2_DONE (bounded) so a QUEUED
// cue (e.g. MAIN-SHOW 2s after PRE-SHOW) can't start talking over his line.
// Pumps MQTT/fleet like waitForMiddle; new cues just queue (s_inCue).
static const uint32_t BURT_DONE_WAIT_MS = 30000;   // his longest line ~25s (09-17)
static void cueBurtAndWait(bool hold) {
    (void)fleet_narrator2_done();                   // drop any stale DONE first
    FleetHandshake hs = fleet_cue_burt(s_theme, s_tone, hold);
    if (!hs.success) {                              // Burt never got it: no 30s of dead air
        Serial.println(F("[cue] Burt did not ACK the cue -- not waiting for his DONE"));
        return;
    }
    const uint32_t t0 = millis();
    bool done = false;
    while (millis() - t0 < BURT_DONE_WAIT_MS) {
        if (fleet_narrator2_done()) { done = true; break; }
        mqtt_tick();
        fleet_tick();
        delay(10);
    }
    Serial.printf("[cue] Burt %s after %lu ms\n", done ? "DONE" : "no DONE (timeout)",
                  (unsigned long)(millis() - t0));
}

static void waitForMiddle() {
    if (s_middleDelivered) return;
    if ((int32_t)(millis() - s_aiDeadline) >= 0) {
        Serial.println(F("[cue] MIDDLE wait: skipped (bridge: none coming / deadline passed)"));
        return;
    }
    const uint32_t session = s_session;
    const uint32_t t0 = millis();
    uint32_t tries = 0;
    const char* why = "timeout";
    for (;;) {
        tries++;
        if (middle_check_and_play(BRIDGE_URL, "sc", session, nullptr) == MID_PLAYED) {
            s_middleDelivered = true;
            why = "played";
            break;
        }
        uint32_t next = millis() + MIDDLE_POLL_MS;
        while ((int32_t)(millis() - next) < 0) {
            mqtt_tick();
            fleet_tick();
            delay(10);
        }
        if (s_session != session)                                            { why = "new params"; break; }
        if ((int32_t)(millis() - s_aiDeadline) >= 0)                          { why = "bridge: none coming"; break; }
        if (cueQueued("mainshow") || cueQueued("reset") || cueQueued("preshow")) { why = "cue queued"; break; }
        if (millis() - t0 >= MIDDLE_WAIT_MS)                                  break;
    }
    Serial.printf("[cue] MIDDLE wait: %s after %lu ms (%lu checks)\n",
                  why, (unsigned long)(millis() - t0), (unsigned long)tries);
}

// Real MQTT-driven cue phases (docs/a1s-board-and-audio-architecture.md §3.2,
// docs/fleet-communication-tiers.md, docs/TASK-a1s-hold-segment.md Part B) —
// the front-yard trigger path. Independent of the bench serial commands above.
static void doPreshow() {
    char bkt[20]; pools_bucket_name(s_theme, s_tone, bkt, sizeof(bkt));
    Serial.printf("\n[cue] ===== preshow  session=0x%08lX bucket=%s =====\n",
                  (unsigned long)s_session, bkt);
    if (!s_paOn) setPA(true);

    // A new group is starting: a previous visit's SHOW_ACTIVE window must not
    // auto-reset (SHOW_OFF/IDLE) in the middle of THIS cycle. The new
    // SHOW_NARRATOR burst below tells the props a new show is beginning.
    if (s_showActive) {
        Serial.println(F("[cue] new PRE-SHOW during the previous SHOW_ACTIVE window -- window closed"));
        s_showActive = false;
    }
    fleet_broadcast_theme_burst(s_theme, s_tone);
    playPoolOr(SEG_INTRO, s_phIntro, s_phIntroLen);   // the whole MIDDLE-call buffer -- uninterruptible

    // 2026-09-25 (#5): bounded wait, replaces the single instant check. The
    // bridge builds MIDDLE during the INTRO; play it the moment it lands, up
    // to MIDDLE_WAIT_MS. Not there by then -> fall through, no filler line;
    // HOLD/MAIN-SHOW still each get their own check.
    waitForMiddle();

    fleet_broadcast_narrator_done(1);
    cueBurtAndWait(false);
    audio_out_flush();
    Serial.println(F("[cue] ===== preshow done =====\n"));
}

// No theme burst, no SHOW_STATE(ACTIVE) — a HOLD retrigger reuses the
// already-locked theme/tone (TASK-a1s-hold-segment.md Part B).
static void doHold() {
    Serial.printf("\n[cue] ===== hold  session=0x%08lX (same theme/tone) =====\n",
                  (unsigned long)s_session);

    // Rule 3 bonus: MIDDLE may have landed late (after PRESHOW already moved
    // on). A hit here is a bonus ("oh -- now I really see you") -- HOLD's own
    // 40s taunt cadence/cap fires regardless either way, completely untouched.
    if (!s_middleDelivered && (int32_t)(millis() - s_aiDeadline) < 0) {
        MiddleResult mid = middle_check_and_play(BRIDGE_URL, "sc", s_session, nullptr);
        if (mid == MID_PLAYED) s_middleDelivered = true;
    }

    playPoolOr(SEG_HOLD, nullptr, 0);
    cueBurtAndWait(true);
    audio_out_flush();
    Serial.println(F("[cue] ===== hold done =====\n"));
}

static void doMainshow() {
    Serial.printf("\n[cue] ===== mainshow  session=0x%08lX =====\n", (unsigned long)s_session);
    fleet_broadcast_start_scene(s_theme);

    // Rule 3 bonus, same as doHold(): MIDDLE may not have landed until now.
    if (!s_middleDelivered && (int32_t)(millis() - s_aiDeadline) < 0) {
        MiddleResult mid = middle_check_and_play(BRIDGE_URL, "sc", s_session, nullptr);
        if (mid == MID_PLAYED) s_middleDelivered = true;
    }

    // 2026-09-24 OUTRO = Scarecrow/Burt BANTER exchange chosen by the bridge
    // (docs/DESIGN-outro-banter-playback.md). Scarecrow conducts it line by
    // line; it stops itself on any failed turn. Only when NO banter line was
    // heard at all (no exchange in params / not on card / Burt unreachable)
    // does the old matched OUTRO pair play as the fallback. The retired live
    // AI OUTRO (_sc_outro/_bu_outro) checks are gone.
    bool banterHeard = false;
    bool burtUnreachable = false;
    if (s_banterPool >= 0) {
        BanterResult br = banter_run((uint8_t)s_banterPool, s_banterIdx, s_theme, s_tone, s_session);
        banterHeard     = br.played > 0;
        burtUnreachable = !strcmp(br.outcome, "burt_unreachable");
    } else {
        Serial.println(F("[cue] mainshow: no banter exchange in params -> OUTRO pool fallback"));
    }

    if (!banterHeard) {
        playOutroMatchedOr(s_phOutro, s_phOutroLen);   // fail-safe, matched-pair pool (2026-09-22)
        fleet_broadcast_narrator_done(1);
        if (burtUnreachable) {
            // Burt just failed a presence probe: don't cue him and then sit
            // 30s waiting on a DONE that can't come.
            Serial.println(F("[cue] mainshow: Burt unreachable -> skipping his OUTRO line"));
        } else {
            (void)fleet_narrator2_done();                 // drop any stale DONE first
            fleet_cue_burt(s_theme, s_tone, false);
            // Gate SHOW_ACTIVE on Burt's real NARRATOR_2_DONE, bounded (30s:
            // his longest measured OUTRO line ~25s, 2026-09-17).
            uint32_t t0 = millis();
            while (!fleet_narrator2_done() && millis() - t0 < 30000) delay(10);
        }
    } else {
        fleet_broadcast_narrator_done(1);
    }

    fleet_broadcast_show_state(SHOW_ACTIVE);
    s_showActive      = true;
    s_showActiveUntil = millis() + SHOW_ACTIVE_MS;
    audio_out_flush();
    Serial.println(F("[cue] ===== mainshow done — SHOW_ACTIVE window started =====\n"));
}

// Banter-only encore for a group still lingering on the walkway (bridge
// decides; max 2 per group). No START_SCENE, no props, no SHOW_ACTIVE music.
static void doEncore() {
    Serial.printf("\n[cue] ===== encore  session=0x%08lX =====\n", (unsigned long)s_session);
    if (s_banterPool < 0) {
        Serial.println(F("[cue] encore: no banter exchange in params -- nothing played (logged)"));
        return;
    }
    if (!s_paOn) setPA(true);
    banter_run((uint8_t)s_banterPool, s_banterIdx, s_theme, s_tone, s_session);
    audio_out_flush();
    Serial.println(F("[cue] ===== encore done =====\n"));
}

static void doReset() {
    Serial.println(F("[cue] ===== reset ====="));
    fleet_broadcast_show_state(SHOW_OFF);
    delay(2000);
    fleet_broadcast_show_state(SHOW_IDLE);
    s_showActive = false;
    s_haveParams = false;
}

static void setPAcb(bool on) { setPA(on); }

static void runCue(const ShowCue& c);

static void onShowCue(const ShowCue& c) {
    // 2026-09-25: "nomiddle" = the bridge's MIDDLE assembly failed after it had
    // said one was coming. Not a show phase: never queued or run, it just ends
    // the post-INTRO wait now (arrives during waitForMiddle's mqtt_tick).
    if (!strcmp(c.phase, "nomiddle")) {
        if (c.session_id == s_session) {
            s_aiDeadline = millis();
            Serial.printf("[mqtt] nomiddle for session 0x%08lX -- MIDDLE wait ends now\n",
                          (unsigned long)c.session_id);
        } else {
            Serial.printf("[mqtt] nomiddle for stale session 0x%08lX -- ignored\n",
                          (unsigned long)c.session_id);
        }
        return;
    }
    if (s_inCue) {
        if (s_cueQLen < 4) {
            s_cueQ[(s_cueQHead + s_cueQLen) % 4] = c;
            s_cueQLen++;
            Serial.printf("[mqtt] cue '%s' arrived mid-cue -- queued (%u)\n", c.phase, s_cueQLen);
        } else {
            Serial.printf("[mqtt] cue queue FULL -- dropped '%s'\n", c.phase);
        }
        return;
    }
    s_inCue = true;
    runCue(c);
    s_inCue = false;
}

static void runCue(const ShowCue& c) {
    s_lastCueMs = millis();
    Serial.printf("[mqtt] cue: session=0x%08lX phase=%s\n",
                  (unsigned long)c.session_id, c.phase);
    if (!s_haveParams) {
        Serial.println(F("[cue] no params latched yet — ignoring"));
        return;
    }
    if      (!strcmp(c.phase, "preshow"))  doPreshow();
    else if (!strcmp(c.phase, "hold"))     doHold();
    else if (!strcmp(c.phase, "mainshow")) doMainshow();
    else if (!strcmp(c.phase, "encore"))   doEncore();
    else if (!strcmp(c.phase, "reset")) {
        // 2026-09-24: the bridge publishes "reset" right behind "mainshow"
        // (its cycle is done), but for the PROPS the show is just starting:
        // SHOW_ACTIVE opens the 210s theme-music window (zombie run, Singing
        // Pumpkins, ~3:00 theme tracks). Resetting here broadcast SHOW_OFF +
        // SHOW_IDLE ~2s after the narrators finished and cut all of it. While
        // that window is open, defer: the window's own expiry (loop()) sends
        // the real reset. A reset for a cycle that never reached MAIN-SHOW
        // (no window open) still runs immediately.
        if (s_showActive && (int32_t)(millis() - s_showActiveUntil) < 0) {
            Serial.printf("[cue] reset DEFERRED: SHOW_ACTIVE window open, %lus left "
                          "(props keep playing; auto-reset at window end)\n",
                          (unsigned long)((s_showActiveUntil - millis()) / 1000));
        } else {
            doReset();
        }
    }
    else { Serial.printf("[cue] unknown phase '%s' — ignored\n", c.phase); return; }

    // Publish immediately on cue completion, not just the 30s heartbeat timer
    // (main loop() below) -- added 2026-09-17. The bridge's own "possible
    // miss" self-check (service.py) only waits 8s for a status change; a cue
    // that took 10-15s of blocking OUTRO playback to complete could never
    // show up in time on the old 30s-only cadence, so a fully successful
    // cue could still get logged as a false "possible miss". This call is in
    // addition to, not a replacement for, the periodic one in loop().
    publishStatus();
}

static void handleSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        switch (c) {
            case 't': narratorSelfTest(false); break;
            case 'T': narratorSelfTest(true);  break;
            case 'n': advanceCue();
                      { char b[20]; pools_bucket_name(s_theme, s_tone, b, sizeof(b));
                        Serial.printf("[main] bucket -> %s\n", b); } break;
            case 'c': advanceCue(); runCycle(); break;
            case 'C': s_session = 1; fleet_set_session(1);      // deterministic MIDDLE URL for bench
                      s_theme = THEME_ROTLD; s_tone = MODE_SCARY;
                      Serial.println(F("[main] fixed-session cycle (0x00000001)"));
                      runCycle(); break;
            case 'a': s_autoCycle = !s_autoCycle;
                      Serial.printf("[main] auto-cycle = %d\n", s_autoCycle); break;
            case 'x': fleet_coex_test(BRIDGE_URL "/coex_probe.bin", false); break;
            case 'X': fleet_coex_test(BRIDGE_URL "/coex_probe.bin", true);  break;
            case 'b': fleet_cue_burt(s_theme, s_tone); break;
            case 'A': aside_force(); break;   // bench: one tree aside now (still needs idle)
            case 'e': {   // bench: run banter exchange  e<pool 0-3><idx>, e.g. "e0 12" = scary #12
                int pool = Serial.parseInt(); int idx = Serial.parseInt();
                if (!s_paOn) setPA(true);
                banter_run((uint8_t)pool, (uint16_t)idx, s_theme, s_tone, s_session);
                break;
            }
            case 'h': fleet_print_handshake_log(); break;
            case 'w': netStatus(); break;
            case 'd': {
                for (int th = 0; th < 3; th++) for (int to = 0; to < 2; to++) {
                    char b[20]; pools_bucket_name((ShowTheme)th, (ShowMode)to, b, sizeof(b));
                    Serial.printf("   %-18s intro=%d outro=%d hold=%d\n", b,
                                  pools_count(SEG_INTRO, (ShowTheme)th, (ShowMode)to),
                                  pools_count(SEG_OUTRO, (ShowTheme)th, (ShowMode)to),
                                  pools_count(SEG_HOLD,  (ShowTheme)th, (ShowMode)to));
                }
                Serial.printf("   bridge/generic=%d  mounted=%d  pool_version=%lu\n",
                              pools_count(SEG_BRIDGE, s_theme, s_tone),
                              pools_mounted(), (unsigned long)pools_version());
                break;
            }
            case 'r': es8388_dump_regs(); break;
            case 'p': setPA(!s_paOn); break;
            case '+': s_volume = min<int>(100, s_volume + 10); es8388_set_volume(s_volume);
                      Serial.printf("[main] vol %u%%\n", s_volume); break;
            case '-': s_volume = max<int>(0, s_volume - 10); es8388_set_volume(s_volume);
                      Serial.printf("[main] vol %u%%\n", s_volume); break;
            default: break;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println(F("================================================================="));
    Serial.println(F(" Scarecrow Narrator A1S — first-test build (real SD pools + HOLD)"));
    Serial.println(F("================================================================="));
    Serial.printf("[main] %s rev%d  heap %u  PSRAM %u\n", ESP.getChipModel(),
                  ESP.getChipRevision(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize());

    pinMode(A1S_PA_EN, OUTPUT);
    setPA(false);

    Serial.print(F("[main] ES8388 init... "));
    bool codec = es8388_init(A1S_I2C_SDA, A1S_I2C_SCL, ES_OUT_ALL);
    Serial.println(codec ? F("OK") : F("FAILED"));
    if (codec) es8388_set_volume(s_volume);

    Serial.print(F("[main] I2S init... "));
    Serial.println(audio_out_begin(BASE_RATE) ? F("OK") : F("FAILED"));

    Serial.println(F("[main] SD pools..."));
    bool sd = pools_begin();
    if (sd) aside_begin();
    if (!sd) {
        s_phIntroLen = 44 + BASE_RATE * 2 * 2;
        s_phOutroLen = 44 + (uint32_t)(16000 * 1.4) * 2;
        s_phIntro = (uint8_t*)ps_malloc(s_phIntroLen);
        s_phOutro = (uint8_t*)ps_malloc(s_phOutroLen);
        if (s_phIntro) s_phIntroLen = wav_make_tone(s_phIntro, s_phIntroLen, BASE_RATE, 523, 2000, 0.5f);
        if (s_phOutro) s_phOutroLen = wav_make_tone(s_phOutro, s_phOutroLen, 16000, 415, 1400, 0.5f);
        Serial.printf("[main] placeholder WAVs in PSRAM: intro %uB outro %uB\n",
                      (unsigned)s_phIntroLen, (unsigned)s_phOutroLen);
    }

    s_session = esp_random();
    Serial.println(F("[main] fleet (WiFi + ESP-NOW)..."));
    fleet_begin(s_session);
    netStatus();

    ota_begin();

    mqtt_set_params_callback(onShowParams);
    mqtt_set_cue_callback(onShowCue);
    mqtt_begin();

    fleet_broadcast_node_online();

    Serial.println(F("[main] ready — auto-cycle OFF."));
    Serial.println(F("[main]   t=self-test (INTRO>MIDDLE>OUTRO)   T=self-test w/ HOLD"));
    Serial.println(F("[main]   n=next bucket   d=pool table   +/- =volume   p=amp"));
    Serial.println(F("[main]   c=full cycle   a=auto-cycle (2-board)   x/X=coex  b/h/w=fleet"));
}

void loop() {
    handleSerial();
    fleet_tick();
    fleet_heartbeat_tick();
    ota_tick();
    mqtt_tick();

    // 2026-09-25: SD not mounted -> retry while idle (a reseated card comes
    // back without a power cycle); state rides leader/status "sd".
    static uint32_t sdRetry = 0;
    if (!s_inCue && !pools_mounted() && millis() - sdRetry >= 20000) {
        sdRetry = millis();
        Serial.println(F("[pools] !! SD not mounted -- retrying mount"));
        if (pools_begin()) Serial.println(F("[pools] SD RECOVERED -- pools live"));
    }

    if (s_cueQLen && !s_inCue) {
        ShowCue c = s_cueQ[s_cueQHead];
        s_cueQHead = (s_cueQHead + 1) % 4;
        s_cueQLen--;
        Serial.printf("[mqtt] running queued cue '%s'\n", c.phase);
        onShowCue(c);
    }

    if (s_showActive && (int32_t)(millis() - s_showActiveUntil) >= 0) {
        Serial.println(F("[cue] SHOW_ACTIVE_MS expired — auto reset"));
        doReset();
    }

    if (fleet_node_online_seen() && s_haveParams) {
        Serial.println(F("[fleet] answering CMD_NODE_ONLINE with a fresh theme burst"));
        fleet_broadcast_theme_burst(s_theme, s_tone);
    }

    // Tree aside: only when nothing is happening with the show (no visit
    // latched, no SHOW_ACTIVE window, no bench auto-cycle, quiet since the last
    // cue). Short and interruptible -- see aside.h.
    {
        bool idle = !s_haveParams && !s_showActive && !s_autoCycle &&
                    (s_lastCueMs == 0 || millis() - s_lastCueMs >= ASIDE_QUIET_AFTER_CUE_MS);
        aside_tick(idle, s_paOn, setPAcb);
    }

    static uint32_t lastStatus = 0;
    if (millis() - lastStatus >= 30000) {
        lastStatus = millis();
        publishStatus();
    }

    static uint32_t last = 0;
    static bool firstDone = false;
    uint32_t interval = firstDone ? AUTO_CYCLE_MS : FIRST_CYCLE_MS;
    if (s_autoCycle && millis() - last >= interval) {
        last = millis();
        firstDone = true;
        advanceCue();
        runCycle();
    }
    delay(5);
}
