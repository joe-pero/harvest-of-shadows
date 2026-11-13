// =============================================================================
// Skeleton Narrator A1S ("Burt") — follower firmware
// =============================================================================
// The Tier-2 partner of the Scarecrow (docs/fleet-communication-tiers.md T2-1).
// No MQTT, no fleet-cue origination, no leader heartbeat. Burt:
//   1. latches (theme,tone) off the fleet SET_THEME/SET_MODE broadcast
//   2. waits for CMD_NARRATOR_2_START (broadcast burst OR the Scarecrow's
//      unicast copy — §3.4 H1b cue direction)
//   3. on the cue: unicasts a CMD_ACK "starting" back to the Scarecrow (native
//      confirmation, <=3 tries, non-blocking — H1b handoff direction), then
//      plays his line
//   4. broadcasts CMD_NARRATOR_2_DONE (3x)
//
// Validated with placeholder audio (no microSD card yet — same as Scarecrow).
//
// Serial:  s = speak an INTRO line   S = speak a HOLD line   o = speak an OUTRO line
//          n = next (theme,tone) bucket   (all from the real SD pool, NO cue needed)
//          l = link log   w = net status   d = pool table   t = test tone
//          p = toggle on-board amp   + / - = volume
//
// OPPORTUNISTIC FLAVOR/OUTRO (2026-09-18/19, NOT YET BENCH-VERIFIED): Burt's
// FIRST-EVER HTTP request. serviceCue() now checks middle_check_and_play()
// (middle.h/.cpp) for a live-generated "bu.wav" flavor line (foreknowledge/
// hearsay reaction, reusing Scarecrow's observation) at every phase, and for
// a "bu_outro.wav" AI-outro payoff at MAIN-SHOW specifically, where it's now
// PRIMARY -- playLine()'s prerecorded OUTRO segment is the fail-safe for a
// broken/late/abandoned AI return, not always-both. Neither check ever gates
// or delays playLine() itself. See docs/_history/TASK-a1s-middle-parts-wiring.md,
// "Burt's side — same pattern" for the full design.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include "pins_a1s.h"
#include "es8388.h"
#include "audio_out.h"
#include "wav.h"
#include "pools.h"
#include "burt_link.h"
#include "NarratorBanter.h"
#include "middle.h"
#include "ota.h"
#include "ProjectConstants.h"
#include "wifi_credentials.h"

static const uint32_t BASE_RATE = 22050;
#define BRIDGE_URL "http://192.168.1.x:8080"     // audio_bridge.py :8080, same as Scarecrow
static uint8_t  s_volume = 99;   // was 65 -- bumped 2026-09-17, Joe: audio too quiet
static bool     s_paOn   = false;

// Mirrors Scarecrow's AI_DEADLINE_MS (main.cpp, scarecrow-narrator-a1s) --
// raised from 120000 to 200000 2026-09-22 for the same reason: real measured
// MIDDLE-pipeline latency (29-243s, median 101.6s) made the old 2-min cutoff
// unreachable past the 2nd HOLD tick. Keep these two constants in step.
static const uint32_t AI_DEADLINE_MS = 200000UL;

// Opportunistic flavor-line/AI-outro delivery state (2026-09-18/19, docs/
// docs/_history/TASK-a1s-middle-parts-wiring.md "Burt's side — same pattern"). Burt has no
// separate params-latch event like Scarecrow's onShowParams() -- a new
// BurtCue.session showing up in serviceCue() IS the "new session" signal, so
// that's where these reset. s_burtAiDeadline is the shared 2-min dead-man's
// switch (mirrors Scarecrow's s_aiDeadline) for BOTH files.
static uint32_t s_burtSession          = 0;
static bool     s_burtFlavorDelivered  = false;
static uint32_t s_burtAiDeadline       = 0;

// local bucket for the standalone voice self-test (s / S / o / n)
static ShowTheme s_theme = THEME_CORN;
static ShowMode  s_tone  = MODE_SCARY;

static uint8_t* s_phLine = nullptr;  static size_t s_phLineLen = 0;

static void setPA(bool on) {
    s_paOn = on;
    digitalWrite(A1S_PA_EN, on ? HIGH : LOW);
    Serial.printf("[main] on-board amp (GPIO21) = %s\n", on ? "HIGH" : "LOW");
}

static void playLine(const BurtCue& cue) {
    PoolSegment seg = cue.hold ? SEG_HOLD : cue.mainshow ? SEG_OUTRO : SEG_INTRO;
    const char* lbl = cue.hold ? "HOLD" : cue.mainshow ? "OUTRO" : "INTRO";
    // 2026-09-22: OUTRO uses the matched-pair pool (session-derived index,
    // same line Scarecrow's board lands on) -- INTRO/HOLD stay on the
    // ordinary random pools_play(), unchanged. See pools.cpp.
    int r = cue.mainshow
        ? pools_play_matched(seg, cue.theme, cue.tone, cue.session, nullptr)
        : pools_play(seg, cue.theme, cue.tone, nullptr);
    if      (r == 2) Serial.printf("[burt] line: %s pool\n", lbl);
    else if (r == 1) Serial.printf("[burt] line: %s bridge fallback\n", lbl);
    else {
        Serial.printf("[burt] line: %s no SD pool -> placeholder WAV\n", lbl);
        if (s_phLine) wav_play_mem(s_phLine, s_phLineLen, nullptr);
        else          audio_out_silence_ms(900);
    }
}

// Standalone voice check — play one real pool line for the current (theme,tone),
// no cue from the Scarecrow required. Enables the on-board amp.
static void burtSelfPlay(PoolSegment seg) {
    char bkt[20];
    pools_bucket_name(s_theme, s_tone, bkt, sizeof(bkt));
    const char* lbl = seg == SEG_INTRO ? "INTRO" : seg == SEG_HOLD ? "HOLD" : "OUTRO";
    Serial.printf("\n[selftest] Burt %s  bucket=%s\n", lbl, bkt);
    if (!s_paOn) setPA(true);
    int r = pools_play(seg, s_theme, s_tone, nullptr);
    Serial.printf("[selftest] %s\n", r == 2 ? "pool line" : r == 1 ? "bridge fallback" : "nothing played");
    audio_out_flush();
}

static void serviceCue() {
    BurtCue cue;
    if (!burt_take_cue(cue)) return;

    // New session (first cue seen for it) -- reset delivery state + start the
    // dead-man's switch, same clock shape as Scarecrow's s_aiDeadline.
    if (cue.session != s_burtSession) {
        s_burtSession         = cue.session;
        s_burtFlavorDelivered = false;
        s_burtAiDeadline      = millis() + AI_DEADLINE_MS;
    }

    // H1b handoff: tell the Scarecrow we're starting (diagnostic; show never waits)
    burt_send_starting_ack(cue.session);

    // 2026-09-24 OUTRO banter turn: play EXACTLY the cued line, nothing else
    // (no MIDDLE check here -- his MIDDLE line must never land mid-conversation),
    // then answer with a DONE that echoes the turn + whether it played, so the
    // Scarecrow can tell a real finish from a missing file. Never silent.
    if (cue.banter) {
        if (!s_paOn) setPA(true);
        char path[64];
        snprintf(path, sizeof(path), "/burt/banter/%s/%03u_%u.wav",
                 BANTER_POOL_NAMES[banter_pool(cue.turnBits)], banter_idx(cue.turnBits),
                 banter_line(cue.turnBits));
        bool played = pools_play_file(path) == 1;
        burt_broadcast_turn_done(cue.turnBits, played);
        return;
    }

    // Opportunistic MIDDLE bonus ("bu.wav" = his matched template line,
    // 2026-09-24) -- checked at pre-show/HOLD/main-show cues, never gates
    // playLine() below.
    if (!s_burtFlavorDelivered && (int32_t)(millis() - s_burtAiDeadline) < 0) {
        MiddleResult flavor = middle_check_and_play(BRIDGE_URL, "bu", cue.session, nullptr);
        if (flavor == MID_PLAYED) s_burtFlavorDelivered = true;
    }

    // His own line (INTRO / HOLD / main-show OUTRO fallback). The retired live
    // AI OUTRO ("bu_outro") check is gone.
    playLine(cue);
    burt_broadcast_narrator_done(cue.mainshow);
}

static void handleSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        switch (c) {
            case 's': burtSelfPlay(SEG_INTRO); break;
            case 'S': burtSelfPlay(SEG_HOLD);  break;
            case 'o': burtSelfPlay(SEG_OUTRO); break;
            case 'n': {
                static uint8_t k = 0; k = (k + 1) % 6;
                s_theme = (ShowTheme)(k / 2);
                s_tone  = (k & 1) ? MODE_FRIENDLY : MODE_SCARY;
                char b[20]; pools_bucket_name(s_theme, s_tone, b, sizeof(b));
                Serial.printf("[main] bucket -> %s\n", b);
                break;
            }
            case 'l': burt_print_log(); break;
            case 'w': Serial.printf("[net] wifi_up=%d ch=%d (want %d) ip=%s heap=%u psram=%u\n",
                                    burt_wifi_up(), burt_wifi_channel(), WIFI_CHANNEL, burt_ip(),
                                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram()); break;
            case 'd': {
                for (int th = 0; th < 3; th++) for (int to = 0; to < 2; to++) {
                    char b[20]; pools_bucket_name((ShowTheme)th, (ShowMode)to, b, sizeof(b));
                    Serial.printf("   %-18s intro=%d outro=%d hold=%d\n", b,
                                  pools_count(SEG_INTRO, (ShowTheme)th, (ShowMode)to),
                                  pools_count(SEG_OUTRO, (ShowTheme)th, (ShowMode)to),
                                  pools_count(SEG_HOLD,  (ShowTheme)th, (ShowMode)to));
                }
                Serial.printf("   bridge/generic=%d mounted=%d\n",
                              pools_count(SEG_BRIDGE, THEME_CORN, MODE_SCARY), pools_mounted());
                break;
            }
            case 't': audio_out_tone(392, 200, 0.5f); audio_out_tone(523, 300, 0.5f); break;
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
    Serial.println(F(" Skeleton Narrator A1S (\"Burt\") — follower firmware"));
    Serial.println(F("================================================================="));
    Serial.printf("[main] %s rev%d  heap %u  PSRAM %u\n", ESP.getChipModel(),
                  ESP.getChipRevision(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize());

    pinMode(A1S_PA_EN, OUTPUT);
    setPA(false);

    // MAC sanity check (non-halting — Burt has no MAC-specific behaviour, but a
    // mismatch means this firmware is on the wrong board).
    {
        WiFi.mode(WIFI_STA);
        String mac = WiFi.macAddress();
        char want[18];
        snprintf(want, sizeof(want), "%02X:%02X:%02X:%02X:%02X:%02X",
                 MAC_SKELETON_A1S[0], MAC_SKELETON_A1S[1], MAC_SKELETON_A1S[2],
                 MAC_SKELETON_A1S[3], MAC_SKELETON_A1S[4], MAC_SKELETON_A1S[5]);
        bool ok = mac.equalsIgnoreCase(want);
        Serial.printf("[main] MAC %s  (want %s) %s\n", mac.c_str(), want,
                      ok ? "-- Burt, OK" : "-- !! MISMATCH — wrong board?");
    }

    Serial.print(F("[main] ES8388 init... "));
    bool codec = es8388_init(A1S_I2C_SDA, A1S_I2C_SCL, ES_OUT_ALL);
    Serial.println(codec ? F("OK") : F("FAILED"));
    if (codec) es8388_set_volume(s_volume);

    Serial.print(F("[main] I2S init... "));
    Serial.println(audio_out_begin(BASE_RATE) ? F("OK") : F("FAILED"));

    Serial.println(F("[main] SD pools..."));
    if (!pools_begin()) {
        s_phLineLen = 44 + BASE_RATE * 2 * 2;   // ~2 s
        s_phLine = (uint8_t*)ps_malloc(s_phLineLen);
        if (s_phLine) s_phLineLen = wav_make_tone(s_phLine, s_phLineLen, BASE_RATE, 294, 1800, 0.5f);
        Serial.printf("[main] placeholder line WAV in PSRAM: %uB (D4, 1.8 s)\n", (unsigned)s_phLineLen);
    }

    Serial.println(F("[main] link (WiFi + ESP-NOW)..."));
    burt_begin();

    ota_begin();

    Serial.println(F("[main] ready — waiting for CMD_NARRATOR_2_START from the Scarecrow."));
    Serial.println(F("[main] standalone voice check: s=INTRO  S=HOLD  o=OUTRO  n=next bucket"));
    Serial.println(F("[main] cmds: l w d t p +/-"));
}

void loop() {
    handleSerial();
    burt_tick();
    ota_tick();
    serviceCue();

    // 2026-09-25: SD not mounted (seen live: 0x107 at boot, fixed by a reseat)
    // -> keep retrying while idle, so a reseated card comes back without a
    // power cycle. Until then lines are the placeholder tone and every ACK
    // tells the Scarecrow SD=FAILED (leader/status "burt_sd").
    static uint32_t sdRetry = 0;
    if (!pools_mounted() && millis() - sdRetry >= 20000) {
        sdRetry = millis();
        Serial.println(F("[pools] !! SD not mounted -- retrying mount"));
        if (pools_begin()) Serial.println(F("[pools] SD RECOVERED -- pools live"));
    }

    static uint32_t alive = 0;
    if (millis() - alive >= 20000) {
        alive = millis();
        burt_print_log();
    }
    delay(5);
}
