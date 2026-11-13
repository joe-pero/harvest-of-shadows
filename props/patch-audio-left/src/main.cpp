// =============================================================================
// Spatial audio patch node (patch LEFT / patch RIGHT) — Tier 1, listen-only
// =============================================================================
// docs/patch-audio-nodes.md · docs/mp3-yx5300-driver.md (never-stuck checklist)
//
// Rebuilt 2026-10-01 on the Spooky Tree's base (Joe: "same concept as the tree,
// it's the default logic for these mp3 players"): shared SafeYX5300 driver,
// ShowFollower.h, black box, self-heal, slow cold boot, noise-proof serial.
// One ESP32 + one YX5300 + SD + a single powered speaker (one channel of the
// pre-mixed stereo theme soundscape). No sensors. Pure follower:
//
//   idle (every night)          -> 001.mp3 ambient bed, replayed on every wrap
//   Scarecrow SHOW_ACTIVE        -> the latched theme's track once (002 corn /
//                                   003 ROTLD / 004 mutant pumpkins), via
//                                   ShowFollower (track end + fail-safe deadlines)
//   theme over / STOP / failure  -> back to the ambient bed
//
// LEFT vs RIGHT is ONLY the SD-card content (+ NodeID). The firmware is
// identical — build env `patch_left` or `patch_right` sets THIS_NODE.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include "BlackBox.h"
#include "NodeDiag.h"
#include "SelfHeal.h"
#include "ProjectConstants.h"
#include "ShowFollower.h"
#include "espnow_rx.h"
#include "mp3_player.h"

// Not #if: NODE_PATCH_* are enum values, which the preprocessor sees as 0, so
// `#if THIS_NODE == NODE_PATCH_LEFT` was always true (Patch Right logged itself
// as "PATCH LEFT", 2026-10-01). Decided at compile time by the compiler instead.
static_assert(THIS_NODE == NODE_PATCH_LEFT || THIS_NODE == NODE_PATCH_RIGHT, "THIS_NODE must be a patch node");
static constexpr const char* SIDE_NAME = (THIS_NODE == NODE_PATCH_LEFT) ? "PATCH LEFT" : "PATCH RIGHT";

static void printTs() { logTs(); }

// =============================================================================
// Playback state
// =============================================================================

enum PlayState : uint8_t { PLAY_AMBIENT, PLAY_THEME, PLAY_IDLE };
static const char* const kState[] = { "AMBIENT", "THEME", "IDLE" };

static PlayState g_playState       = PLAY_IDLE;
static uint32_t  g_ambientRetryAtMs = 0;   // back-off after a failed ambient start
static uint32_t  g_bootMs           = 0;

#define AMBIENT_RETRY_BACKOFF_MS  30000UL
#define LOOP_WDT_S                60        // loop() must come round at least this often
// Boot settle (Tree 2026-10-01): after a real power-up nothing is touched for
// 20 s, then the MP3 module, then the radio (its calibration draws a spike).
#define BOOT_SETTLE_COLD_MS       20000UL
#define BOOT_SETTLE_WARM_MS        3000UL

// ── Show follower ─────────────────────────────────────────────────────────────
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
    c.expectedTrackMs[THEME_CORN]            = LEN_THEME_MS;
    c.expectedTrackMs[THEME_ROTLD]           = LEN_THEME_MS;
    c.expectedTrackMs[THEME_MUTANT_PUMPKINS] = LEN_THEME_MS;
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
        case THEME_MUTANT_PUMPKINS: return TRACK_THEME_PUMPKINS;
        default:                    return TRACK_THEME_CORN;
    }
}

// ── Deferred mp3 op (onEspNowCmd must never block) ───────────────────────────
enum PendingMp3Op : uint8_t { MP3_OP_NONE, MP3_OP_STOP, MP3_OP_PLAY };
static PendingMp3Op g_pendingMp3Op    = MP3_OP_NONE;
static uint8_t      g_pendingMp3Track = 0;

// =============================================================================
// Black box + self-heal
// =============================================================================

static SerialLineReader  g_serial;
static RxFrameLog        g_rxLog;
static LoopGap           g_loopGap;
static selfheal::Monitor g_heal;

static void printStatus(uint32_t now, bool full) {
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
                          "modStop=%u stale=%u fileErr=%u modErr=%u reset=%u silent=%u loopRecover=%u loopWrap=%u idx=%u | "
                          "radio=%s txOk=%lu chRelock=%lu\n",
                          (unsigned)mp3CurrentTrack(), m.plays, m.startRetries, m.startFails, m.stopResends, m.defuses,
                          m.lengthStops, m.moduleStops, m.staleEnds, m.fileErrors, m.moduleErrors, m.moduleResets,
                          m.silentWarns, m.loopRecovers, m.loopWraps, m.indexMismatch,
                          es.ready ? "up" : "DOWN", (unsigned long)es.txOk, (unsigned long)es.chRelocks);
    if (es.rxBadLen || es.rxBadSeal) {
        printTs(); Log.printf("[rx] last junk frame: len=%d from %02X:%02X:%02X:%02X:%02X:%02X (not a fleet Msg, dropped)\n",
                              es.lastBadLen, es.lastBadMac[0], es.lastBadMac[1], es.lastBadMac[2],
                              es.lastBadMac[3], es.lastBadMac[4], es.lastBadMac[5]);
    }
}

// =============================================================================
// Bench commands over USB serial (noise-proof input; any command holds
// self-heal 10 min and turns the live echo on for 30 min)
// =============================================================================
//   visit corn|rotld|mutant — NEW session: SET_THEME + SHOW_NARRATOR (PRE-SHOW)
//   corn / rotld / mutant   — SET_THEME only
//   active                  — SHOW_ACTIVE          → theme track starts/replaces
//   off / idle              — SHOW_OFF / SHOW_IDLE → ends the run only after 3:00
//   hb                      — CMD_HEARTBEAT (liveness)
//   fake <cmd>              — same frame from a non-Scarecrow sender (must be ignored)
//   t <n> / stop            — play track n (1 ambient, 2-4 theme) / stop (ambient back in 5 s)
//   status                  — full status block now
//   log / log tail [KB] / log stat / log mark <text> / log clear yes — black box
//   bbtest reboot|panic|hang — the next boot must recover this line from RAM
//   heal off / heal on      — self-heal reboots off/on (until reboot)
static bool startTrack(uint8_t track);
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
    else if (th >= 0)          benchSend(CMD_SET_THEME, th, sender);
    else if (line == "active") benchSend(CMD_SHOW_STATE, SHOW_ACTIVE, sender);
    else if (line == "off")    benchSend(CMD_SHOW_STATE, SHOW_OFF, sender);
    else if (line == "idle")   benchSend(CMD_SHOW_STATE, SHOW_IDLE, sender);
    else if (line == "hb")     benchSend(CMD_HEARTBEAT, 0, sender);
    else return false;
    return true;
}

static void checkSerialTestCommand() {
    String line;
    while (g_serial.poll(line)) {
        if (line.length()) g_heal.benchActivity(millis());
        if (benchInjectCue(line)) {
            // handled
        } else if (line.startsWith("t ")) {
            const int t = line.substring(2).toInt();
            if (t < TRACK_AMBIENT || t > TRACK_THEME_PUMPKINS) { printTs(); Log.println("[bench] t 1..4"); }
            else {
                g_follow.abort();
                printTs(); Log.printf("[bench] Manual play track %d\n", t);
                g_playState = startTrack((uint8_t)t) ? (t == TRACK_AMBIENT ? PLAY_AMBIENT : PLAY_THEME) : PLAY_IDLE;
            }
        } else if (line == "stop") {
            printTs(); Log.println("[bench] Manual stop — ambient resumes in 5 s");
            g_follow.abort();
            mp3Stop("bench stop");
            g_playState        = PLAY_IDLE;
            g_ambientRetryAtMs = millis() + 5000;
        } else if (Log.command(line)) {
            // log / log tail / log stat / log mark / log clear yes / bbtest
        } else if (line == "heal off" || line == "heal on") {
            g_heal.setEnabled(line == "heal on");
            printTs(); Log.printf("[heal] self-heal %s\n", g_heal.enabled() ? "ON" : "OFF (until reboot or heal on)");
        } else if (line == "status") {
            printStatus(millis(), true);
        } else {
            g_serial.unknown(line);
        }
    }
}

// Apply one ShowFollower answer. Sets intent only; the blocking mp3 call runs
// once in loop().
static void applyFollower(const showfollow::Out& o) {
    if (o.why) {
        printTs(); Log.printf("[show]%s %s  (theme=%u session=0x%08lX phase=%s)\n",
                              o.warn ? " WARN" : "", o.why, (unsigned)o.theme,
                              (unsigned long)g_follow.runSession(),
                              showfollow::Follower::phaseName(g_follow.phase()));
    }
    if (o.act == showfollow::Act::PLAY) {
        g_pendingMp3Op    = MP3_OP_PLAY;               // replaces whatever is playing
        g_pendingMp3Track = themeTrack(o.theme);
        g_playState       = PLAY_THEME;
    } else if (o.act == showfollow::Act::TO_IDLE) {
        g_playState        = PLAY_IDLE;                // ambient restarts next tick (its PLAY stops the theme)
        g_ambientRetryAtMs = 0;
    }
}

// ── Stale-visit guard (Tree 2026-09-27) ───────────────────────────────────────
// The Scarecrow answers any CMD_NODE_ONLINE with a theme burst incl.
// SHOW_STATE(NARRATOR) for its LAST session. Our own boot's answer is a resync,
// not a visit; a visit that timed out without SHOW_ACTIVE is dead.
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
        case CMD_STOP:                                 // end any show run, back to ambient
            g_follow.abort();
            g_pendingMp3Op = MP3_OP_STOP;
            g_playState    = PLAY_IDLE;
            printTs(); Log.printf("[main] STOP from node 0x%02X\n", msg.sender);
            break;
        case CMD_OTA_MODE:                             // no OTA on this node (bridge USB only)
            printTs(); Log.printf("[main] OTA request from node 0x%02X ignored (no OTA on the patch nodes)\n", msg.sender);
            break;
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

    const esp_reset_reason_t r = esp_reset_reason();
    Log.begin(SIDE_NAME, resetReasonName(), r == ESP_RST_POWERON);
    SafeYX5300::setLog(Log);
    const uint8_t healCount = healBootsBegin();
    printTs(); Log.printf("[main] %s (NodeID 0x%02X) booting... black-box boot #%u\n",
                          SIDE_NAME, (unsigned)THIS_NODE, (unsigned)Log.bootNo());
    printTs(); Log.printf("[main] Reset reason: %s (%d)%s\n", resetReasonName(), (int)r,
                          r == ESP_RST_BROWNOUT ? "  <-- SUPPLY SAG: check the power feed" :
                          (r == ESP_RST_TASK_WDT || r == ESP_RST_PANIC || r == ESP_RST_INT_WDT)
                              ? "  <-- firmware hang/crash recovered" : "");
    if (healBootThisBoot()) {
        printTs(); Log.printf("[heal] this boot is self-heal reboot %u in a row (see the HEAL REBOOT line above)\n",
                              (unsigned)healCount);
    }
    {   // Wrong-board check (Tree pattern). A side whose MAC isn't recorded yet
        // (all zero in ProjectConstants.h) only logs it, so it can be recorded.
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        const uint8_t* want = (THIS_NODE == NODE_PATCH_LEFT) ? MAC_PATCH_LEFT : MAC_PATCH_RIGHT;
        bool known = false, match = true;
        for (int i = 0; i < 6; i++) { if (want[i]) known = true; if (want[i] != mac[i]) match = false; }
        printTs(); Log.printf("[main] MAC %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        if (!known) {
            Log.printf(" — not recorded yet: add it as MAC_%s in ProjectConstants.h\n",
                       THIS_NODE == NODE_PATCH_LEFT ? "PATCH_LEFT" : "PATCH_RIGHT");
        } else if (match) {
            Log.printf(" verified — %s\n", SIDE_NAME);
        } else {
            Log.printf(" — [FATAL] MAC MISMATCH: expected %02X:%02X:%02X:%02X:%02X:%02X (%s). Wrong firmware on this board, halted.\n",
                       want[0], want[1], want[2], want[3], want[4], want[5], SIDE_NAME);
            Log.sync();
            pinMode(2, OUTPUT);
            while (true) { digitalWrite(2, HIGH); delay(500); digitalWrite(2, LOW); delay(500); }
        }
    }

    const bool cold = r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT || r == ESP_RST_UNKNOWN;
    const uint32_t settleMs = cold ? BOOT_SETTLE_COLD_MS : BOOT_SETTLE_WARM_MS;
    printTs(); Log.printf("[main] %s boot: settling %lu s before the MP3 module and the radio\n",
                          cold ? "Cold" : "Warm", (unsigned long)(settleMs / 1000));
    Log.sync();
    delay(settleMs);
    mp3Init();
    if (mp3GetLastActivityMs()) { printTs(); Log.println("[main] MP3 module answering"); }
    else { printTs(); Log.println("[main] WARN MP3 module NOT answering after the handshake (power? wiring?) — retries + self-heal will follow"); }

    espnowInit();                 // radio last
    espnowSetCallback(onEspNowCmd);

    const esp_err_t wdtErr = esp_task_wdt_init(LOOP_WDT_S, true);
    enableLoopWDT();
    printTs(); Log.printf("[main] Loop watchdog %s (%d s, err=%d)\n",
                          wdtErr == ESP_OK ? "armed" : "WARN init failed", LOOP_WDT_S, (int)wdtErr);

    g_bootMs = millis();
    espnowSendNodeOnline();
    printTs(); Log.printf("[main] Boot complete in %lu s. Ambient bed; following the Scarecrow\n",
                          (unsigned long)(millis() / 1000));
}

// =============================================================================
// loop
// =============================================================================

static bool startTrack(uint8_t track) {
    const bool ok = (track == TRACK_AMBIENT) ? mp3PlayLoop(track) : mp3PlayTrack(track);
    if (!ok) {
        mp3ClearFinished();
        printTs(); Log.printf("[main] WARN track %u did not start (%s)\n",
                              (unsigned)track, SafeYX5300::endName(mp3EndReason()));
    }
    return ok;
}

// Self-heal: safe = only the node's own ambient bed (no theme run, no visit,
// nothing pending). A reboot then costs ~15 s of ambient.
static void selfHealTick() {
    const uint32_t now  = millis();
    const bool     safe = (g_playState == PLAY_AMBIENT || g_playState == PLAY_IDLE) && !g_follow.isPlaying() &&
                          !g_follow.inVisit() && g_pendingMp3Op == MP3_OP_NONE;
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
        printTs(); Log.printf("[heal] WARN %s for %lus — %s (state=%s track=%u)\n", selfheal::whyName(w),
                              (unsigned long)(g_heal.pendingForMs(now) / 1000), held,
                              kState[g_playState], (unsigned)mp3CurrentTrack());
    } else if (w == selfheal::Why::NONE && before != selfheal::Why::NONE) {
        printTs(); Log.printf("[heal] %s cleared on its own — no reboot\n", selfheal::whyName(before));
    }
    if (d.why != selfheal::Why::NONE) {
        const uint32_t lastRx = mp3GetLastActivityMs();
        printTs(); Log.printf("[heal] HEAL REBOOT: %s%s — module last answered %s%lus ago, state=%s track=%u, "
                              "startFails=%u churn=%u, self-heal boot %u in a row\n",
                              selfheal::whyName(d.why), d.forced ? " (FORCED: module dead 10 min mid-show)" : "",
                              lastRx ? "" : "never/", (unsigned long)((now - (lastRx ? lastRx : g_bootMs)) / 1000),
                              kState[g_playState], (unsigned)mp3CurrentTrack(), (unsigned)g_heal.startFails(),
                              (unsigned)g_heal.churnCount(now), (unsigned)(healBoots() + 1));
        printStatus(now, true);
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

    checkSerialTestCommand();

    {   // a 1-line black-box record every minute, the full block every 5
        static uint32_t s_statusMs = 0, s_statusN = 0;
        if (now - s_statusMs >= 60000) {
            s_statusMs = now;
            printStatus(now, (s_statusN++ % 5) == 0);
        }
    }

    espnowTick();  // fully drains the inbound queue; handlers only set intent

    if (g_pendingMp3Op != MP3_OP_NONE) {
        const PendingMp3Op op = g_pendingMp3Op;
        g_pendingMp3Op = MP3_OP_NONE;
        if (op == MP3_OP_STOP) {
            mp3Stop("STOP command");
        } else if (op == MP3_OP_PLAY) {
            if (!startTrack(g_pendingMp3Track) && g_follow.isPlaying())
                applyFollower(g_follow.onPlayFailed(millis()));
        }
    }

    espnowHeartbeatTick();
    mp3Tick();

    // ── Show follower: the theme run's end + fail-safe timers ─────────────────
    if (g_follow.isPlaying() && g_playState == PLAY_THEME && mp3IsFinished()) {
        const Mp3End why = mp3EndReason();
        mp3ClearFinished();
        if (why == Mp3End::FILE_ERROR || why == Mp3End::START_FAILED)
            applyFollower(g_follow.onPlayFailed(millis()));
        else
            applyFollower(g_follow.onTrackEnd(millis()));
    }
    applyFollower(g_follow.tick(millis()));
    checkVisitTimeout();

    // A bench theme (t 2..4, not a follower run) ended -> ambient.
    if (!g_follow.isPlaying() && g_playState == PLAY_THEME && mp3IsFinished()) {
        mp3ClearFinished();
        g_playState = PLAY_IDLE;
    }

    // ── Idle: the ambient bed ─────────────────────────────────────────────────
    if (g_playState == PLAY_IDLE && (int32_t)(now - g_ambientRetryAtMs) >= 0) {
        if (startTrack(TRACK_AMBIENT)) {
            g_playState = PLAY_AMBIENT;
            g_heal.idleStartOk();
        } else {
            g_ambientRetryAtMs = now + AMBIENT_RETRY_BACKOFF_MS;
            g_heal.idleStartFailed();
        }
    }
    if (g_playState == PLAY_AMBIENT && mp3IsFinished()) {
        // The driver replays the bed on every wrap; it only "finishes" if the
        // module stopped / reset / failed -> restart it at once.
        printTs(); Log.printf("[main] ambient bed ended (%s) — restarting\n", SafeYX5300::endName(mp3EndReason()));
        mp3ClearFinished();
        g_heal.idleBedDied(now);
        g_playState = PLAY_IDLE;
    }

    selfHealTick();
    delay(10);
}
