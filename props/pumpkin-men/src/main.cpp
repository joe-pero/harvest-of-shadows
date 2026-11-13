#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include "BlackBox.h"
#include "NodeDiag.h"
#include "SelfHeal.h"
#include "ProjectConstants.h"
#include "ShowFollower.h"
#include "wifi_credentials.h"
#include "mp3_player.h"
#include "relay_effects.h"
#include "espnow_handler.h"
#include "pir_sensor.h"

static void printTs() { logTs(); }

// =============================================================================
// MAC self-check — halts with LED blink if firmware is on the wrong board
// =============================================================================

static bool verifyNodeMac() {
    char expected[18];
    snprintf(expected, sizeof(expected), "%02X:%02X:%02X:%02X:%02X:%02X",
             MAC_PUMPKIN_MEN[0], MAC_PUMPKIN_MEN[1], MAC_PUMPKIN_MEN[2],
             MAC_PUMPKIN_MEN[3], MAC_PUMPKIN_MEN[4], MAC_PUMPKIN_MEN[5]);

    // From the eFuse (2026-10-01): the radio now starts after the MP3 module.
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char actualBuf[18];
    snprintf(actualBuf, sizeof(actualBuf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    String actual = actualBuf;
    if (actual.equalsIgnoreCase(expected)) {
        printTs(); Log.printf("[OK] MAC verified: %s — Pumpkin Men\n", expected);
        return true;
    }

    printTs(); Log.printf("[FATAL] MAC MISMATCH — wrong firmware on this board!\n");
    printTs(); Log.printf("[FATAL] Expected: %s (from ProjectConstants.h, carried over from\n");
    printTs(); Log.printf("[FATAL]           the 2025 MAC_MASTER — confirm this is the same\n");
    printTs(); Log.printf("[FATAL]           physical ACEBOTT box before re-flashing)\n");
    printTs(); Log.printf("[FATAL] Got:      %s\n", actual.c_str());

    pinMode(2, OUTPUT);
    while (true) {
        digitalWrite(2, HIGH); delay(500);
        digitalWrite(2, LOW);  delay(500);
    }
    return false;
}

// =============================================================================
// Playback state
// =============================================================================
// Main Switch Controller retired 2026-09-24 (Joe): no SHOW_LOCK / SHOW_UNLOCK /
// TRIGGER handling any more. Theme tracks play only when the Scarecrow's show
// says so (ShowFollower below), or from the bench.
//
// Idle action (Joe, 2026-09-25; 2026-09-26: short clips only): between shows the
// GPIO36 PIR plays a short clip (004..018, jaws only) dealt from a Fisher-Yates deck, so no clip repeats
// until all 15 have played. Rules: never while a theme run plays; held while a visit's narrators
// talk (and a running action is cut when a visit starts); a show cue always
// replaces it; re-arms ACTION_COOLDOWN_MS after the action ends.

// PLAY_LOOP (Joe, 2026-09-26): track 019 (was 020 until 10-07) repeats gaplessly on the module whenever
// nothing else plays. A theme or action replaces it (their PLAY sends STOP
// first); once they end, loop() starts it again. No effects during the loop.
enum PlayState : uint8_t { PLAY_IDLE, PLAY_THEME, PLAY_ACTION, PLAY_LOOP };

#define LOOP_RESUME_MS      1000UL    // after a theme/action ends
#define LOOP_RETRY_FAST_MS  3000UL    // first retry after the loop died / failed
#define LOOP_RETRY_MS      30000UL    // later retries (module keeps failing)
static uint8_t  s_loopFails   = 0;
static bool     s_loopEnabled = true;  // bench `loop off` for a quiet bench
static uint32_t s_loopNextMs  = 0;

static PlayState g_playState  = PLAY_IDLE;

#define ACTION_COOLDOWN_MS   90000UL   // re-arm this long after an action ends

static uint32_t  g_pirReadyMs     = 0;  // PIR warm-up end
static uint32_t  g_actionRearmMs  = 0;  // next action allowed at/after this

// ── Short-clip shuffle deck (Fisher-Yates, same as Spooky Tree's action deck) ─
#define CLIP_DECK_SIZE (TRACK_CLIP_LAST - TRACK_CLIP_FIRST + 1)
static uint8_t s_clipDeck[CLIP_DECK_SIZE];
static uint8_t s_clipDeckPos  = CLIP_DECK_SIZE;   // exhausted -> shuffle on first deal
static uint8_t s_lastClip     = 0;

static uint8_t dealClip() {
    if (s_clipDeckPos >= CLIP_DECK_SIZE) {
        for (uint8_t i = 0; i < CLIP_DECK_SIZE; i++) s_clipDeck[i] = TRACK_CLIP_FIRST + i;
        for (uint8_t i = CLIP_DECK_SIZE - 1; i > 0; i--) {
            uint8_t j = (uint8_t)random(i + 1);
            uint8_t t = s_clipDeck[i]; s_clipDeck[i] = s_clipDeck[j]; s_clipDeck[j] = t;
        }
        if (s_clipDeck[0] == s_lastClip) {             // no repeat across the reshuffle
            uint8_t t = s_clipDeck[0]; s_clipDeck[0] = s_clipDeck[1]; s_clipDeck[1] = t;
        }
        s_clipDeckPos = 0;
    }
    s_lastClip = s_clipDeck[s_clipDeckPos++];
    return s_lastClip;
}

// PIR actions are the short clips only (jaws only, no fog).
static uint8_t pickActionTrack() {
    return dealClip();
}

// Blocking (mp3 pacing) — call from loop()/bench only, never from the ESP-NOW drain.
static void startAction(uint8_t track, const char* why) {
    printTs(); Log.printf("[action] %s -> track %u (short clip)\n", why, (unsigned)track);
    if (mp3PlayTrack(track)) {
        relayEffectsStart(Fx::CLIP, mp3TrackLengthMs(track));
        g_playState = PLAY_ACTION;
    } else {
        mp3ClearFinished();
        relayEffectsStop();
        g_playState    = PLAY_IDLE;
        g_actionRearmMs = millis() + ACTION_COOLDOWN_MS;   // don't hammer a failing module
        printTs(); Log.println("[action] track did not start (see [mp3] lines)");
    }
}

static void endAction(const char* why, bool stopAudio) {
    if (stopAudio) mp3Stop(why); else mp3ClearFinished();
    relayEffectsStop();
    g_playState     = PLAY_IDLE;
    g_actionRearmMs = millis() + ACTION_COOLDOWN_MS;
    printTs(); Log.printf("[action] %s — re-arms in %lus\n", why, (unsigned long)(ACTION_COOLDOWN_MS / 1000));
}

static uint32_t  g_otaServiceUntilMs = 0;
static bool      g_otaActive         = false;

// ── Deferred mp3 op ────────────────────────────────────────────────────────────
// Same reasoning as Spooky Tree: onEspNowCmd() must never block. The MP3 driver
// paces every command (500 ms) and confirms starts; running it inline would
// stall draining the rest of a frame burst (the Scarecrow's 3x cue groups).
enum PendingMp3Op : uint8_t { MP3_OP_NONE, MP3_OP_STOP, MP3_OP_PLAY };
static PendingMp3Op g_pendingMp3Op    = MP3_OP_NONE;
static uint8_t       g_pendingMp3Track = 0;

// ── Show follower (show-sync-orchestrator, 2026-09-24) ──────────────────────
// The Scarecrow is the show controller. shared/Shared_Constants/ShowFollower.h
// holds the whole cue decision (host-tested: props/_tests/run.sh), identical on
// Spooky Tree. This node only applies its answers:
//   PLAY(theme) → theme track + relay effects, replacing whatever plays
//   TO_IDLE     → stop audio + relays (Pumpkin Men has no idle audio)
static_assert(showfollow::W_CMD_START_SCENE == CMD_START_SCENE &&
              showfollow::W_CMD_SET_THEME == CMD_SET_THEME &&
              showfollow::W_CMD_SHOW_STATE == CMD_SHOW_STATE &&
              showfollow::W_CMD_HEARTBEAT == CMD_HEARTBEAT &&
              showfollow::W_NODE_SCARECROW == NODE_SCARECROW_A1S &&
              showfollow::W_SHOW_NARRATOR == SHOW_NARRATOR &&
              showfollow::W_SHOW_ACTIVE == SHOW_ACTIVE &&
              showfollow::W_SHOW_OFF == SHOW_OFF && showfollow::W_SHOW_IDLE == SHOW_IDLE &&
              showfollow::W_THEME_COUNT == THEME_MUTANT_PUMPKINS + 1,
              "ShowFollower.h wire values drifted from ProjectConstants.h");

static showfollow::Config followCfg() {
    showfollow::Config c;
    c.showActiveMs = SHOW_ACTIVE_MS;
    // Planned house length: every theme track trimmed to 3:00 (Joe, 2026-09-24).
    // Mutant Pumpkins = 003 "New mutant" 3:00 (Joe 2026-10-07; was the 2:09 action scene).
    c.expectedTrackMs[THEME_CORN]            = 180000;
    c.expectedTrackMs[THEME_ROTLD]           = 180000;
    c.expectedTrackMs[THEME_MUTANT_PUMPKINS] = 180000;
    // R3 off (2026-10-27): a Scarecrow resync burst (answer to any NODE_ONLINE) carries
    // the encore's new session + SHOW_NARRATOR and cut a playing theme ~60 s in
    // (Tree 10-26 19:26). A theme now ends at its 3:00 or on the next real SHOW_ACTIVE.
    c.stopOnNewNarrator = false;
    return c;
}
static showfollow::Follower g_follow(followCfg());

static uint8_t themeTrack(uint8_t theme) {
    switch (theme) {
        case THEME_ROTLD:           return TRACK_THEME_ROTLD;
        case THEME_MUTANT_PUMPKINS: return TRACK_THEME_MPUMP;    // 003 New mutant (Joe 2026-10-07)
        default:                    return TRACK_THEME_CORN;
    }
}

// Apply one ShowFollower answer. Sets intent only; the blocking mp3/relay calls
// run once in loop(), so it is safe from the ESP-NOW drain and from loop().
static void applyFollower(const showfollow::Out& o) {
    if (o.why) {
        printTs(); Log.printf("[show]%s %s  (theme=%u session=0x%08lX phase=%s)\n",
                                 o.warn ? " WARN" : "", o.why, (unsigned)o.theme,
                                 (unsigned long)g_follow.runSession(),
                                 showfollow::Follower::phaseName(g_follow.phase()));
    }
    if (o.act == showfollow::Act::PLAY) {
        g_pendingMp3Op    = MP3_OP_PLAY;       // replaces whatever is playing; relays restart
        g_pendingMp3Track = themeTrack(o.theme);
        g_playState       = PLAY_THEME;
    } else if (o.act == showfollow::Act::TO_IDLE) {
        g_pendingMp3Op = MP3_OP_STOP;          // audio + relays off
        g_playState    = PLAY_IDLE;
    }
}

// =============================================================================
// OTA service mode — WiFi connects only while active
// =============================================================================

static void otaServiceStop() {
    ArduinoOTA.end();
    WiFi.disconnect(true);
    delay(100);
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    g_otaActive         = false;
    g_otaServiceUntilMs = 0;
    enableLoopWDT();
    printTs(); Log.println("[main] OTA service mode ended — channel restored");
}

static void otaServiceStart(uint32_t durationMin) {
    if (g_otaActive) return;
    printTs(); Log.printf("[main] OTA service mode starting — %u min window\n", (unsigned)durationMin);
    disableLoopWDT();               // an OTA transfer blocks inside ArduinoOTA.handle()

    mp3Stop("OTA service mode");
    relayEffectsStop();
    g_follow.abort();
    g_playState = PLAY_IDLE;

    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    printTs(); Log.print("[main] WiFi connecting");
    uint32_t wifiTimeout = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < wifiTimeout) {
        delay(500);
        Serial.print(".");          // progress dots: serial only, not worth the log
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        printTs(); Log.println("[main] WiFi connect failed — aborting OTA");
        WiFi.disconnect(true);
        delay(100);
        esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
        enableLoopWDT();
        return;
    }

    printTs(); Log.printf("[main] WiFi connected — IP %s\n", WiFi.localIP().toString().c_str());

    ArduinoOTA.setHostname("halloween-pumpkin-men");
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.begin();

    g_otaActive         = true;
    g_otaServiceUntilMs = millis() + durationMin * 60000UL;
    printTs(); Log.printf("[main] OTA ready — expires at t=%ums\n", (unsigned)g_otaServiceUntilMs);
}

// =============================================================================
// Black box + self-heal (2026-10-01, ported from the Spooky Tree; checklist
// items 8-11 in docs/mp3-yx5300-driver.md). Every minute one compact [bb] line;
// `full` adds the follower / MP3-counter block (every 5 min, and `status`).
// =============================================================================

#define LOOP_WDT_S          60        // loop() must come round at least this often
#define BOOT_SETTLE_COLD_MS 20000UL   // after a real power-up: settle before MP3 + radio
#define BOOT_SETTLE_WARM_MS  3000UL   // after a soft reset (as before)

static SerialLineReader  g_serial;
static RxFrameLog        g_rxLog;
static LoopGap           g_loopGap;
static selfheal::Monitor g_heal;
static uint32_t          g_bootMs = 0;

static void printStatus(uint32_t now, bool full) {
    static const char* const kState[] = { "IDLE", "THEME", "ACTION", "LOOP" };
    const uint32_t lastRx = mp3GetLastActivityMs();
    const EspNowStats es = espnowStats();
    char heal[48];
    if (!g_heal.enabled()) snprintf(heal, sizeof(heal), "OFF");
    else if (g_heal.pending() != selfheal::Why::NONE)
        snprintf(heal, sizeof(heal), "%s/%lus%s", selfheal::whyName(g_heal.pending()),
                 (unsigned long)(g_heal.pendingForMs(now) / 1000), g_heal.benchHeld(now) ? "/bench" : "");
    else snprintf(heal, sizeof(heal), "ok");
    if (healBoots()) { size_t l = strlen(heal); snprintf(heal + l, sizeof(heal) - l, " healBoots=%u", (unsigned)healBoots()); }
    printTs(); Log.printf("[bb] up=%lum st=%s trk=%u mp3rx=%s%lus heal=%s heap=%u/%u loopMax=%lums slow=%lu | "
                          "rx=%lu q=%lu rep=%lu badLen=%lu badSeal=%lu notUs=%lu drop=%lu sc=%s%lus ch=%u txFail=%lu | uartJunk=%lu/%lu\n",
                          (unsigned long)(now / 60000), kState[g_playState], (unsigned)mp3CurrentTrack(),
                          lastRx ? "" : "never/",
                          (unsigned long)(max((int32_t)0, (int32_t)(now - (lastRx ? lastRx : g_bootMs))) / 1000),
                          heal, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                          (unsigned long)g_loopGap.maxMs, (unsigned long)g_loopGap.slow,
                          (unsigned long)es.rxAll, (unsigned long)es.rxQueued, (unsigned long)g_rxLog.repeats,
                          (unsigned long)es.rxBadLen, (unsigned long)es.rxBadSeal, (unsigned long)es.rxNotUs,
                          (unsigned long)es.qDropped, es.lastScarecrowMs ? "" : "never/",
                          (unsigned long)(es.lastScarecrowMs ? (now - es.lastScarecrowMs) / 1000 : 0),
                          (unsigned)es.channel, (unsigned long)es.txFail,
                          (unsigned long)g_serial.junkMin, (unsigned long)g_serial.junk);
    g_serial.junkMin = 0;
    g_loopGap.maxMs  = 0;
    if (!full) return;

    const showfollow::Stats& st = g_follow.stats();
    printTs(); Log.printf("[show] %s theme=%u run=%lus leader=%s | plays=%u repl=%u dup=%u "
                          "earlyEOT=%u earlyOFF=%u deadline=%u unconf=%u lenWarn=%u playFail=%u badSender=%u visitTimeouts=%u\n",
                          showfollow::Follower::phaseName(g_follow.phase()), (unsigned)g_follow.theme(),
                          (unsigned long)(g_follow.runElapsed(now) / 1000),
                          g_follow.leaderSilent(now) ? "SILENT" : "ok",
                          st.plays, st.replaces, st.dupIgnored, st.earlyEotIgnored,
                          st.earlyOffIgnored, st.deadlineHits, st.themeUnconfirmed,
                          st.durationWarns, st.playFails, st.badSender, st.visitTimeouts);
    const SafeYX5300Stats& m = mp3Stats();
    printTs(); Log.printf("[mp3] track=%u plays=%u retry=%u startFail=%u stopResend=%u defuse=%u lenStop=%u "
                          "modStop=%u stale=%u fileErr=%u modErr=%u reset=%u silent=%u loopRecover=%u loopWrap=%u idx=%u | txOk=%lu\n",
                          (unsigned)mp3CurrentTrack(), m.plays, m.startRetries, m.startFails, m.stopResends, m.defuses,
                          m.lengthStops, m.moduleStops, m.staleEnds, m.fileErrors, m.moduleErrors, m.moduleResets,
                          m.silentWarns, m.loopRecovers, m.loopWraps, m.indexMismatch, (unsigned long)es.txOk);
    if (es.rxBadLen || es.rxBadSeal) {
        printTs(); Log.printf("[rx] last junk frame: len=%d from %02X:%02X:%02X:%02X:%02X:%02X (not a fleet Msg, dropped)\n",
                              es.lastBadLen, es.lastBadMac[0], es.lastBadMac[1], es.lastBadMac[2],
                              es.lastBadMac[3], es.lastBadMac[4], es.lastBadMac[5]);
    }
}

// ── Bench-only test commands over USB serial ─────────────────────────────────
// Added for bringup before the fleet mesh is live — verify each of the 3
// theme tracks and each relay individually once Joe's tracks are on the SD
// card, without needing a real ESP-NOW broadcast. Physical-USB-only, same
// bench-debug convention as Spooky Tree ("ota") and the Scarecrow A1S self-test
// commands (t/T/n).
//   1 / 2 / 3   — play that theme track + arm its relay effects
//   stop        — stop playback + force all relays off
//   jaw1..jaw5  — pulse one jaw (1 skullL 2 skullM 3 skullR 4 witchM 5 witchR)
//   eyes        — witch eyes candle flicker 5 s
//   jawlog / jawlog off — print every jaw start
//   fog         — pulse the fog relay in isolation
//   reach       — reaching men ON 5 s (GPIO26, NC relay)
//   ota         — open the 15-min OTA service window
//   pir         — simulate a PIR detection (skips warm-up + cooldown, same rules otherwise)
//   clip        — play the next short clip from the deck directly
//   t <n>       — play track n (1..18) with its show effects; t 19 = start the idle loop
//   loop off/on — idle loop off (quiet bench) / back on
//   chase       — each jaw + fog ON 2 s in turn, labelled (channel map)
//   pin <n>     — any safe GPIO HIGH 2 s (find an unknown MOSFET channel)
//   pirwatch    — toggle printing every raw GPIO36 level change
//   piroff/piron — disable/enable PIR idle actions (bench quiet)
// Show-cue simulation (2026-09-24, show-sync-orchestrator): feeds synthetic
// Scarecrow frames through the REAL onEspNowCmd() → ShowFollower path, so a
// bench run over the bridge Pi exercises exactly what a live show does.
//   visit corn|rotld|mutant — NEW session: SET_THEME + SHOW_NARRATOR (PRE-SHOW)
//   corn / rotld / mutant   — SET_THEME only (same session; mid-run = replace)
//   active                  — SHOW_ACTIVE          → theme track starts/replaces
//   off / idle              — SHOW_OFF / SHOW_IDLE → ends the run only after 3:00
//   hb                      — CMD_HEARTBEAT (liveness)
//   mp3                     — MP3 driver status + counters
//   fake <cmd>              — same frame but from a non-Scarecrow sender (must be ignored)
//   show                    — print follower status
//   status                  — black-box status block now
//   log / log tail [KB] / log stat / log mark <text> / log clear yes — black box (shared/BlackBox)
//   bbtest reboot|panic|hang — bench check: the next boot must recover this line from RAM
//   heal off / heal on      — self-heal reboots off/on (until reboot); any command holds them 10 min
static void onEspNowCmd(const Msg& msg);
static uint32_t s_benchSession = 0xBE7C0000;
static void benchSend(uint8_t cmd, uint32_t a, uint8_t sender) {
    Msg m = {};
    m.command = cmd; m.sender = sender; m.target = NODE_BROADCAST;
    m.session_id = s_benchSession; m.payload_a = a;
    onEspNowCmd(m);
}
static bool benchInjectCue(String line) {
    uint8_t sender = NODE_SCARECROW_A1S;
    if (line.startsWith("fake ")) { sender = NODE_MAIN_SWITCH; line = line.substring(5); }
    int th = -1;
    String arg = line.startsWith("visit ") ? line.substring(6) : line;
    if      (arg == "corn")   th = THEME_CORN;
    else if (arg == "rotld")  th = THEME_ROTLD;
    else if (arg == "mutant") th = THEME_MUTANT_PUMPKINS;

    if (line.startsWith("visit ") && th >= 0) {
        s_benchSession++;
        printTs(); Log.printf("[bench] visit session=0x%08lX\n", (unsigned long)s_benchSession);
        benchSend(CMD_SET_THEME, th, sender);
        benchSend(CMD_SHOW_STATE, SHOW_NARRATOR, sender);
    }
    else if (th >= 0)         benchSend(CMD_SET_THEME, th, sender);
    else if (line == "active") benchSend(CMD_SHOW_STATE, SHOW_ACTIVE, sender);
    else if (line == "off")    benchSend(CMD_SHOW_STATE, SHOW_OFF, sender);
    else if (line == "idle")   benchSend(CMD_SHOW_STATE, SHOW_IDLE, sender);
    else if (line == "hb")     benchSend(CMD_HEARTBEAT, 0, sender);
    else if (line == "mp3") {
        const SafeYX5300Stats& m = mp3Stats();
        printTs(); Log.printf("[mp3] track=%u lastRx=%lus ago | plays=%u retry=%u startFail=%u stopResend=%u "
                                 "defuse=%u lenStop=%u modStop=%u stale=%u fileErr=%u modErr=%u reset=%u silent=%u idx=%u\n",
                                 (unsigned)mp3CurrentTrack(), (unsigned long)((millis() - mp3GetLastActivityMs()) / 1000),
                                 m.plays, m.startRetries, m.startFails, m.stopResends, m.defuses, m.lengthStops,
                                 m.moduleStops, m.staleEnds, m.fileErrors, m.moduleErrors, m.moduleResets, m.silentWarns, m.indexMismatch);
    }
    else if (line == "show") {
        uint32_t now = millis();
        printTs(); Log.printf("[show] phase=%s theme=%u runTheme=%u run=%lus lastPlayed=%lus leader=%s\n",
                                 showfollow::Follower::phaseName(g_follow.phase()), (unsigned)g_follow.theme(),
                                 (unsigned)g_follow.runTheme(), (unsigned long)(g_follow.runElapsed(now) / 1000),
                                 (unsigned long)(g_follow.lastPlayedMs() / 1000),
                                 g_follow.leaderSilent(now) ? "SILENT" : "ok");
    }
    else return false;
    return true;
}

static bool s_benchPir   = false; // one simulated PIR edge, consumed in loop()
static bool s_pirWatch   = false; // bench: print every raw GPIO36 level change
static bool s_pirEnabled = true;  // bench: `piroff` stops PIR actions (bench quiet)

// Input is noise-proof (NodeDiag.h: UART0 RX floats with no USB host). Any real
// command holds self-heal for 10 min (a person is at the bench).
static void checkSerialTestCommand() {
    String s_line;
    while (g_serial.poll(s_line)) {
            if (s_line.length()) g_heal.benchActivity(millis());
            if (benchInjectCue(s_line)) {
                // handled
            } else if (s_line == "1" || s_line == "2" || s_line == "3") {
                uint8_t track = (uint8_t)s_line.toInt();
                printTs(); Log.printf("[bench] Manual play track %u\n", track);
                g_follow.abort();
                if (mp3PlayTrack(track)) {
                    relayEffectsStart(Fx::THEME, mp3TrackLengthMs(track));
                    g_playState = PLAY_THEME;
                } else {
                    mp3ClearFinished();
                    printTs(); Log.println("[bench] track did not start (see [mp3] lines)");
                }
            } else if (s_line == "stop") {
                printTs(); Log.println("[bench] Manual stop");
                g_follow.abort();
                mp3Stop("bench stop");
                relayEffectsStop();
                g_playState = PLAY_IDLE;
            } else if (s_line.startsWith("jaw") && s_line.length() == 4) {
                uint8_t which = (uint8_t)(s_line[3] - '0');
                relayEffectsBenchJaw(which);
            } else if (s_line == "fog") {
                relayEffectsBenchFog();
            } else if (s_line == "jawlog" || s_line == "jawlog off") {
                relayEffectsJawLog(s_line == "jawlog");
                printTs(); Log.printf("[bench] jaw log %s\n", s_line == "jawlog" ? "ON" : "OFF");
            } else if (s_line == "eyes") {
                relayEffectsBenchEyes();
            } else if (s_line == "reach") {
                relayEffectsBenchReach();
            } else if (s_line == "pir") {
                printTs(); Log.println("[bench] Simulated PIR detection");
                g_pirReadyMs = g_actionRearmMs = 0;
                s_benchPir   = true;
            } else if (s_line == "loop off" || s_line == "loop on") {
                s_loopEnabled = (s_line == "loop on");
                if (!s_loopEnabled && g_playState == PLAY_LOOP) { mp3Stop("bench loop off"); g_playState = PLAY_IDLE; }
                s_loopNextMs = millis();
                printTs(); Log.printf("[bench] idle loop %s\n", s_loopEnabled ? "ON" : "OFF (until reboot or loop on)");
            } else if (s_line.startsWith("t ")) {
                // Any track by number, with the effects it gets in the show.
                int t = s_line.substring(2).toInt();
                if (t < 1 || t > TRACK_IDLE_LOOP) { printTs(); Log.printf("[bench] t 1..%u\n", TRACK_IDLE_LOOP); }
                else if (t == TRACK_IDLE_LOOP) {
                    g_follow.abort(); relayEffectsStop();
                    s_loopEnabled = true; s_loopNextMs = millis();
                    if (g_playState != PLAY_LOOP) g_playState = PLAY_IDLE;   // loop() starts it now
                    printTs(); Log.println("[bench] idle loop (019) on");
                }
                else if (t <= TRACK_THEME_MPUMP) {
                    g_follow.abort();
                    printTs(); Log.printf("[bench] Manual play track %d (theme)\n", t);
                    if (mp3PlayTrack(t)) { relayEffectsStart(Fx::THEME, mp3TrackLengthMs(t)); g_playState = PLAY_THEME; }
                    else { mp3ClearFinished(); relayEffectsStop(); g_playState = PLAY_IDLE; }
                } else { g_follow.abort(); startAction((uint8_t)t, "bench"); }
            } else if (s_line == "clip") {
                g_follow.abort();
                startAction(dealClip(), "bench");
            } else if (s_line == "chase") {
                // Each named output ON 2 s in turn, labelled — find which MOSFET
                // channel is which (bench 2026-09-26: Joe saw only 2 of 6 working).
                g_follow.abort(); mp3Stop("bench chase"); relayEffectsStop(); g_playState = PLAY_IDLE;
                static const struct { uint8_t pin; const char* name; } outs[] = {
                    { PIN_JAW_SKULL_L, "skull left jaw" }, { PIN_JAW_SKULL_M, "skull middle jaw" },
                    { PIN_JAW_SKULL_R, "skull right jaw" }, { PIN_JAW_WITCH_M, "witch middle jaw" },
                    { PIN_JAW_WITCH_R, "witch right jaw" }, { PIN_FOG, "fog" } };
                for (auto& o : outs) {
                    printTs(); Log.printf("[bench] chase: %s (GPIO%u) ON 2s\n", o.name, (unsigned)o.pin);
                    digitalWrite(o.pin, HIGH); delay(2000); digitalWrite(o.pin, LOW); delay(1000);
                }
                printTs(); Log.println("[bench] chase done");
            } else if (s_line.startsWith("pin ")) {
                // Drive any GPIO that is safe as an output HIGH for 2 s (maps an
                // unknown MOSFET channel to its pin). Skips flash/UART/MP3/PIR pins.
                int p = s_line.substring(4).toInt();
                static const uint8_t ok[] = { 2, 4, 5, 12, 13, 14, 15, 18, 21, 22, 25, 26, 27, 32, 33 };  // 19/23 damaged
                bool safe = false;
                for (uint8_t q : ok) if (q == p) safe = true;
                if (!safe) { printTs(); Log.printf("[bench] pin %d not allowed (safe: 2 4 5 12-15 18 21 22 25-27 32 33; 19/23 damaged)\n", p); }
                else {
                    printTs(); Log.printf("[bench] GPIO%d HIGH 2s\n", p);
                    pinMode(p, OUTPUT); digitalWrite(p, HIGH); delay(2000); digitalWrite(p, LOW);
                }
            } else if (s_line == "pirwatch") {
                s_pirWatch = !s_pirWatch;
                printTs(); Log.printf("[bench] PIR raw watch %s (GPIO%u now %s)\n", s_pirWatch ? "ON" : "OFF",
                                         (unsigned)PIR_PIN, digitalRead(PIR_PIN) ? "HIGH" : "LOW");
            } else if (s_line == "piroff" || s_line == "piron") {
                s_pirEnabled = (s_line == "piron");
                printTs(); Log.printf("[bench] PIR actions %s\n", s_pirEnabled ? "ENABLED" : "DISABLED (until reboot or piron)");
            } else if (s_line == "ota") {
                printTs(); Log.println("[bench] Manual OTA trigger (\"ota\" command)");
                otaServiceStart(15);
            } else if (Log.command(s_line)) {
                // log / log tail / log stat / log mark / log clear yes / bbtest
            } else if (s_line == "heal off" || s_line == "heal on") {
                g_heal.setEnabled(s_line == "heal on");
                printTs(); Log.printf("[heal] self-heal %s\n", g_heal.enabled() ? "ON" : "OFF (until reboot or heal on)");
            } else if (s_line == "status") {
                printStatus(millis(), true);
            } else {
                g_serial.unknown(s_line);
            }
    }
}

// ── ESP-NOW command handler (called from espnowTick, main-loop context) ───────

// Stale-visit guard (ported 2026-10-01 from the Spooky Tree's 2026-09-27 fix).
// The Scarecrow answers any CMD_NODE_ONLINE with a theme burst incl.
// SHOW_STATE(NARRATOR) for its LAST latched session, even with no show running;
// with this node's old 5-min NODE_ONLINE that kept it in a fake VISIT (PIR
// actions held). A visit that timed out without SHOW_ACTIVE is dead: its
// session never starts a visit again; a real new show brings a new session.
static uint32_t s_narratorSession = 0;
static bool     s_haveDeadSession = false;
static uint32_t s_deadSession     = 0;
static uint32_t s_deadDropped     = 0;
static uint16_t s_seenTimeouts    = 0;

static void checkVisitTimeout() {
    const uint16_t n = g_follow.stats().visitTimeouts;
    if (n == s_seenTimeouts) return;
    s_seenTimeouts = n;
    s_haveDeadSession = true; s_deadSession = s_narratorSession; s_deadDropped = 0;
    printTs(); Log.printf("[show] session 0x%08lX marked dead — its NARRATOR repeats no longer start a visit\n",
                          (unsigned long)s_deadSession);
}

static void onEspNowCmd(const Msg& msg) {
    g_rxLog.log(msg);                            // once per change; repeats counted
    if (msg.command == CMD_SHOW_STATE && msg.payload_a == SHOW_NARRATOR && msg.sender == NODE_SCARECROW_A1S) {
        if (s_haveDeadSession && msg.session_id == s_deadSession) {
            if (s_deadDropped++ % 30 == 0) {
                printTs(); Log.printf("[show] NARRATOR for dead session 0x%08lX ignored (x%lu)\n",
                                      (unsigned long)msg.session_id, (unsigned long)s_deadDropped);
            }
            return;
        }
        // The answer to our own boot NODE_ONLINE is a resync, not a visit starting.
        const uint32_t sent = espnowLastOnlineMs();
        if (sent && (int32_t)(millis() - sent) < 3000) {
            s_haveDeadSession = true; s_deadSession = msg.session_id; s_deadDropped = 1;
            printTs(); Log.printf("[show] NARRATOR 0x%08lX = resync answer to our NODE_ONLINE — no visit\n",
                                  (unsigned long)msg.session_id);
            return;
        }
        s_narratorSession = msg.session_id;
    }
    // Show cues (Scarecrow only — the follower checks the sender) drive the show.
    applyFollower(g_follow.onFrame(msg.command, msg.sender, msg.session_id, msg.payload_a, millis()));

    switch ((Command)msg.command) {

        case CMD_STOP:
            g_pendingMp3Op = MP3_OP_STOP;
            g_playState    = PLAY_IDLE;
            g_follow.abort();
            printTs(); Log.printf("[main] STOP from node 0x%02X\n", msg.sender);
            break;

        case CMD_OTA_MODE: {
            uint32_t durationMin = msg.payload_a;
            if (durationMin == 0) durationMin = 15;
            otaServiceStart(durationMin);
            break;
        }

        default:
            break;
    }
}

// =============================================================================
// setup
// =============================================================================

void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }

    // Black box first, so the boot header + everything after it is kept.
    const esp_reset_reason_t r = esp_reset_reason();
    Log.begin("PUMPKIN MEN", resetReasonName(), r == ESP_RST_POWERON);
    SafeYX5300::setLog(Log);
    const uint8_t healCount = healBootsBegin();
    printTs(); Log.printf("[main] Pumpkin Men booting... black-box boot #%u\n", (unsigned)Log.bootNo());
    printTs(); Log.printf("[main] Reset reason: %s (%d)%s\n", resetReasonName(), (int)r,
                          r == ESP_RST_BROWNOUT ? "  <-- SUPPLY SAG: check the box's power feed" :
                          (r == ESP_RST_TASK_WDT || r == ESP_RST_PANIC || r == ESP_RST_INT_WDT)
                              ? "  <-- firmware hang/crash recovered" : "");
    if (healBootThisBoot()) {
        printTs(); Log.printf("[heal] this boot is self-heal reboot %u in a row (see the HEAL REBOOT line above)\n",
                              (unsigned)healCount);
    }

    verifyNodeMac();            // eFuse MAC: no radio needed
    randomSeed(esp_random());  // seed for the jaw deck, clip deck and 40/60 pick

    relayEffectsInit();         // outputs to a known OFF state first
    pirInit();

    // Settle, then MP3, then radio (2026-10-01, from the Spooky Tree: slow but
    // sure on a real power-up; the radio's calibration spike comes last).
    const bool cold = r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT || r == ESP_RST_UNKNOWN;
    const uint32_t settleMs = cold ? BOOT_SETTLE_COLD_MS : BOOT_SETTLE_WARM_MS;
    printTs(); Log.printf("[main] %s boot: settling %lu s before the MP3 module and the radio\n",
                          cold ? "Cold" : "Warm", (unsigned long)(settleMs / 1000));
    Log.sync();
    delay(settleMs);
    mp3Init();    // shared SafeYX5300: boot dwell + warm handshake (~8 s at 500 ms pacing)
    if (mp3GetLastActivityMs()) { printTs(); Log.println("[main] MP3 module answering"); }
    else { printTs(); Log.println("[main] WARN MP3 module NOT answering after the handshake (main power? wiring?) — retries + self-heal will follow"); }

    espnowInit();               // radio last
    espnowSetCallback(onEspNowCmd);

    espnowSendNodeOnline();
    g_pirReadyMs = millis() + PIR_WARMUP_MS;
    printTs(); Log.printf("[main] PIR (GPIO%u) warm-up — actions enabled in %lus\n",
                             (unsigned)PIR_PIN, (unsigned long)(PIR_WARMUP_MS / 1000));
    // Loop watchdog (never-stuck checklist item 3): loop() must come round every
    // LOOP_WDT_S or the chip reboots. Off during an OTA window (otaServiceStart).
    const esp_err_t wdtErr = esp_task_wdt_init(LOOP_WDT_S, true);
    enableLoopWDT();
    printTs(); Log.printf("[main] Loop watchdog %s (%d s, err=%d)\n",
                          wdtErr == ESP_OK ? "armed" : "WARN init failed", LOOP_WDT_S, (int)wdtErr);
    g_bootMs = millis();
    printTs(); Log.printf("[main] Boot complete in %lu s — following the Scarecrow, PIR idle actions\n",
                          (unsigned long)(millis() / 1000));
    printTs(); Log.println("[main] Bench: 1/2/3 stop jaw1..jaw5 eyes jawlog fog reach chase pin <n> pirwatch piroff/piron ota pir clip t <n> loop off/on | visit <theme> active off idle hb fake show mp3 | status log heal");
}

// =============================================================================
// loop
// =============================================================================

// ── Self-heal (SelfHeal.h, ported from the Spooky Tree 2026-10-01) ────────────
// Safe = only the box's own idle (loop 019 or nothing): no theme run, no visit,
// no PIR action, no OTA, nothing pending. Then a reboot costs ~15 s of the idle
// loop with every relay off (relayEffectsInit at boot), nothing tied to the show.
static void selfHealTick() {
    const uint32_t now  = millis();
    const bool     safe = (g_playState == PLAY_IDLE || g_playState == PLAY_LOOP) && !g_follow.isPlaying() &&
                          !g_follow.inVisit() && g_pendingMp3Op == MP3_OP_NONE && !g_otaActive;
    const selfheal::Why before = g_heal.pending();
    const selfheal::Decision d = g_heal.tick(now, g_bootMs, mp3GetLastActivityMs(), safe, healBoots());
    const selfheal::Why w = g_heal.pending();
    static uint32_t s_heldLogMs = 0;
    if (w != selfheal::Why::NONE && (w != before || now - s_heldLogMs >= 60000) && d.why == selfheal::Why::NONE) {
        s_heldLogMs = now;
        const char* held = !g_heal.enabled() ? "self-heal is OFF" :
                           g_heal.benchHeld(now) ? "held: bench activity in the last 10 min" :
                           healBoots() >= g_heal.config().maxHealBoots ? "NOT rebooting: 3 self-heal reboots in a row, needs a person" :
                           !safe ? "held until the show is idle" : "waiting";
        printTs(); Log.printf("[heal] WARN %s for %lus — %s (state=%u track=%u)\n", selfheal::whyName(w),
                              (unsigned long)(g_heal.pendingForMs(now) / 1000), held,
                              (unsigned)g_playState, (unsigned)mp3CurrentTrack());
    } else if (w == selfheal::Why::NONE && before != selfheal::Why::NONE) {
        printTs(); Log.printf("[heal] %s cleared on its own — no reboot\n", selfheal::whyName(before));
    }
    if (d.why != selfheal::Why::NONE) {
        const uint32_t lastRx = mp3GetLastActivityMs();
        printTs(); Log.printf("[heal] HEAL REBOOT: %s%s — module last answered %s%lus ago, state=%u track=%u, "
                              "startFails=%u churn=%u, self-heal boot %u in a row\n",
                              selfheal::whyName(d.why), d.forced ? " (FORCED: module dead 10 min mid-show)" : "",
                              lastRx ? "" : "never/", (unsigned long)((now - (lastRx ? lastRx : g_bootMs)) / 1000),
                              (unsigned)g_playState, (unsigned)mp3CurrentTrack(), (unsigned)g_heal.startFails(),
                              (unsigned)g_heal.churnCount(now), (unsigned)(healBoots() + 1));
        printStatus(now, true);
        relayEffectsStop();                     // nothing left energised across the reboot
        healBootsMark();
        Log.sync();
        delay(100);
        ESP.restart();
    }
    if (healBoots() && w == selfheal::Why::NONE &&
        (int32_t)(now - g_bootMs) >= (int32_t)g_heal.config().healthyMs) {
        printTs(); Log.printf("[heal] healthy %lu min — self-heal reboot count %u cleared\n",
                              (unsigned long)(g_heal.config().healthyMs / 60000), (unsigned)healBoots());
        healBootsClear();
    }
}

void loop() {
    const uint32_t now = millis();

    g_loopGap.tick(now, (uint8_t)g_playState, mp3CurrentTrack());   // WARNs a >= 5 s block
    Log.tick();

    checkSerialTestCommand();  // BENCH TEST ONLY — see comment above its definition

    // ── OTA service mode guard ────────────────────────────────────────────────
    if (g_otaActive) {
        ArduinoOTA.handle();
        if (now >= g_otaServiceUntilMs) {
            otaServiceStop();
        }
        return;
    }

    // ── Status: a 1-line black-box record every minute, the full block every 5 ──
    {
        static uint32_t s_statusMs = 0, s_statusN = 0;
        if (now - s_statusMs >= 60000) {
            s_statusMs = now;
            printStatus(now, (s_statusN++ % 5) == 0);
        }
    }

    espnowTick();  // fully drains the inbound queue; handlers above only set intent

    // Apply the mp3 op (if any) requested by the drain above — the one place these
    // blocking calls actually run, so a frame burst never stalls mid-dispatch.
    if (g_pendingMp3Op != MP3_OP_NONE) {
        PendingMp3Op op = g_pendingMp3Op;
        g_pendingMp3Op  = MP3_OP_NONE;
        switch (op) {
            case MP3_OP_STOP:
                mp3Stop("show / STOP");
                relayEffectsStop();
                break;
            case MP3_OP_PLAY:
                if (mp3PlayTrack(g_pendingMp3Track)) {
                    relayEffectsStart(Fx::THEME, mp3TrackLengthMs(g_pendingMp3Track));  // effects only with confirmed audio
                } else {
                    mp3ClearFinished();
                    relayEffectsStop();
                    if (g_follow.isPlaying()) applyFollower(g_follow.onPlayFailed(millis()));
                    g_playState = PLAY_IDLE;
                }
                break;
            default: break;
        }
    }

    espnowHeartbeatTick();
    mp3Tick();
    relayEffectsTick();

    // ── Show follower: its run's end-of-track + fail-safe timers ──────────────
    if (g_follow.isPlaying() && g_playState == PLAY_THEME && mp3IsFinished()) {
        Mp3End why = mp3EndReason();
        mp3ClearFinished();
        if (why == Mp3End::FILE_ERROR || why == Mp3End::START_FAILED)
            applyFollower(g_follow.onPlayFailed(millis()));
        else
            applyFollower(g_follow.onTrackEnd(millis()));
    }
    applyFollower(g_follow.tick(millis()));   // fresh: frames handled above stamp later than `now`
    checkVisitTimeout();

    // ── Idle action (PIR) ─────────────────────────────────────────────────────
    if (g_playState == PLAY_ACTION) {
        if (g_follow.inVisit())  endAction("visit started (narrators talking) — action cut", true);
        else if (mp3IsFinished()) endAction("action done", false);
    }
    {
        // Poll every loop so an edge is consumed even when we can't act on it.
        bool motion = pirCheck(relayLastSwitchMs()) && now >= g_pirReadyMs && s_pirEnabled;
        if (s_pirWatch) {
            static int s_rawLast = -1;
            int raw = digitalRead(PIR_PIN);
            if (raw != s_rawLast) {
                s_rawLast = raw;
                printTs(); Log.printf("[pir] raw GPIO%u -> %s\n", (unsigned)PIR_PIN, raw ? "HIGH" : "LOW");
            }
        }
        if (s_benchPir) { s_benchPir = false; motion = true; }
        if (motion && (g_playState == PLAY_IDLE || g_playState == PLAY_LOOP) && !g_follow.isPlaying()) {
            if (g_follow.inVisit()) {
                printTs(); Log.println("[action] PIR — held (narrators talking)");
            } else if ((int32_t)(now - g_actionRearmMs) < 0) {
                // cooldown: ignore quietly (the PIR re-fires every time someone moves)
            } else {
                espnowSendPropMotion();
                startAction(pickActionTrack(), "PIR");
            }
        }
    }

    // ── Bench run finished (not a follower run) ───────────────────────────────
    if (!g_follow.isPlaying() && g_playState == PLAY_THEME && mp3IsFinished()) {
        mp3ClearFinished();
        relayEffectsStop();
        g_playState = PLAY_IDLE;
        printTs(); Log.println("[main] Theme track done — waiting for next cue");
    }

    // ── Idle loop (019) ───────────────────────────────────────────────────────
    if (g_playState == PLAY_LOOP && mp3IsFinished()) {           // module reset / file error / stopped
        Mp3End why = mp3EndReason();
        mp3ClearFinished();
        g_heal.idleBedDied(millis());                            // repeated deaths = LOOP_CHURN self-heal
        g_playState  = PLAY_IDLE;
        uint32_t wait = s_loopFails++ ? LOOP_RETRY_MS : LOOP_RETRY_FAST_MS;
        s_loopNextMs = millis() + wait;
        printTs(); Log.printf("[loop] idle loop ended (%s) — retry in %lus\n",
                                 SafeYX5300::endName(why), (unsigned long)(wait / 1000));
    }
    {
        static PlayState s_prev = PLAY_IDLE;
        if (g_playState == PLAY_IDLE && s_prev != PLAY_IDLE && s_prev != PLAY_LOOP)
            s_loopNextMs = millis() + LOOP_RESUME_MS;             // a theme/action just ended
        s_prev = g_playState;
    }
    if (s_loopEnabled && g_playState == PLAY_IDLE && !g_follow.isPlaying() &&
        g_pendingMp3Op == MP3_OP_NONE && (int32_t)(millis() - s_loopNextMs) >= 0) {
        if (mp3PlayLoop(TRACK_IDLE_LOOP)) {
            g_playState = PLAY_LOOP;
            s_loopFails = 0;
            g_heal.idleStartOk();
            printTs(); Log.println("[loop] idle loop 019 playing");
        } else {
            mp3ClearFinished();
            g_heal.idleStartFailed();
            uint32_t wait = s_loopFails++ ? LOOP_RETRY_MS : LOOP_RETRY_FAST_MS;
            s_loopNextMs = millis() + wait;
            printTs(); Log.printf("[loop] idle loop 019 did not start — retry in %lus\n", (unsigned long)(wait / 1000));
        }
    }

    selfHealTick();

    delay(10);
}
