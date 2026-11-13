#include <Arduino.h>
#include "BlackBox.h"
#include "NodeDiag.h"
#include "SelfHeal.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include "ProjectConstants.h"
#include "ShowFollower.h"
#include "mp3_player.h"
#include "pir_sensor.h"
#include "espnow_handler.h"

static void printTs() { logTs(); }

// =============================================================================
// MAC self-check — halts with LED blink if firmware is on the wrong board
// =============================================================================

static bool verifyNodeMac() {
    char expected[18];
    snprintf(expected, sizeof(expected), "%02X:%02X:%02X:%02X:%02X:%02X",
             MAC_SPOOKY_TREE[0], MAC_SPOOKY_TREE[1], MAC_SPOOKY_TREE[2],
             MAC_SPOOKY_TREE[3], MAC_SPOOKY_TREE[4], MAC_SPOOKY_TREE[5]);

    // From the eFuse (2026-10-01): the radio now starts AFTER the MP3 module, so
    // WiFi.macAddress() isn't available yet. Same value it would report.
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char actualBuf[18];
    snprintf(actualBuf, sizeof(actualBuf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    String actual = actualBuf;
    if (actual.equalsIgnoreCase(expected)) {
        printTs(); Log.printf("[OK] MAC verified: %s — Spooky Tree\n", expected);
        return true;
    }

    printTs(); Log.printf("[FATAL] MAC MISMATCH — wrong firmware on this board!\n");
    printTs(); Log.printf("[FATAL] Expected: %s\n", expected);
    printTs(); Log.printf("[FATAL] Got:      %s\n", actual.c_str());

    pinMode(2, OUTPUT);
    while (true) {
        digitalWrite(2, HIGH); delay(500);
        digitalWrite(2, LOW);  delay(500);
    }
    return false;
}

// =============================================================================
// Playback state machine
// =============================================================================
// Main Switch Controller retired 2026-09-24 (Joe): no SHOW_LOCK / SHOW_UNLOCK /
// TRIGGER / DUCK handling any more. The tree is always in its own idle mode
// (ambient loop + PIR action tracks) except while the Scarecrow's show borrows
// it for one theme track (ShowFollower below).

enum PlayState : uint8_t { PLAY_AMBIENT, PLAY_ACTION, PLAY_THEME, PLAY_IDLE };

RTC_DATA_ATTR static uint8_t g_bootCount = 0;

static PlayState g_playState              = PLAY_IDLE;
static uint32_t  g_pirReadyMs             = 0;
static uint32_t  g_ambientRetryAtMs       = 0;   // back-off after a failed ambient start
static uint32_t  g_bootMs                 = 0;

#define AMBIENT_RETRY_BACKOFF_MS      30000UL
#define LOOP_WDT_S                    60    // loop() must come round at least this often
// Boot settle (2026-10-01, Joe: "I don't care if it's slow to boot"). The tree
// fails when switched on at night and works when restarted later, so after a
// real power-up nothing is touched for 20 s (supply, amp, PIR, SD card settle),
// then the MP3 module is brought up, and only then the radio (its calibration
// draws a current spike). A soft reset (watchdog / self-heal) keeps the old 3 s.
#define BOOT_SETTLE_COLD_MS           20000UL
#define BOOT_SETTLE_WARM_MS            3000UL
// The old 7-min "STALL" module watchdog is replaced by self-heal (SelfHeal.h):
// module silent 2 min / idle start fails / idle loop churn -> logged reboot,
// only while idle (never mid-theme or mid-visit), max 3 in a row.

// ── Show follower (show-sync-orchestrator, 2026-09-24) ──────────────────────
// The Scarecrow is the show controller. shared/Shared_Constants/ShowFollower.h
// holds the whole cue decision (host-tested: props/_tests/run.sh), identical on
// Pumpkin Men. This node only applies its answers:
//   PLAY(theme) → the tree borrows itself: theme track replaces whatever plays
//                 (ambient / action / an older theme track), ambient + PIR pause
//   TO_IDLE     → back to the tree's own idle mode (ambient loop + PIR)
// While a visit's narrators are talking (VISIT) the ambient keeps going and PIR
// still reports motion, but the action track is held off so an elk scream
// can't land on top of them.
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
    // Update per theme if a track is intentionally different.
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
        case THEME_MUTANT_PUMPKINS: return TRACK_THEME_MPUMP;
        default:                    return TRACK_THEME_CORN;
    }
}

// ── Deferred mp3 op ────────────────────────────────────────────────────────────
// onEspNowCmd() must never block (see espnow_handler.h / IMPROVEMENTS.md 1.1):
// the MP3 driver paces every command (500 ms) and confirms starts, so running it
// inline mid-dispatch would stall draining the rest of a frame burst. Handlers
// only set intent here; the blocking call happens once, after the queue drains.
enum PendingMp3Op : uint8_t { MP3_OP_NONE, MP3_OP_STOP, MP3_OP_PLAY };
static PendingMp3Op g_pendingMp3Op    = MP3_OP_NONE;
static uint8_t       g_pendingMp3Track = 0;

// ── Action track shuffle deck (Fisher-Yates, matches 2025 Scarecrow deckNext()) ─

#define ACTION_DECK_SIZE 6
static uint8_t s_actionDeck[ACTION_DECK_SIZE] = {2, 2, 3, 4, 5, 6};
static uint8_t s_deckPos         = ACTION_DECK_SIZE;  // start exhausted → shuffle on first call
static uint8_t s_lastActionTrack = 0;

static void shuffleActionDeck() {
    for (uint8_t i = ACTION_DECK_SIZE - 1; i > 0; i--) {
        uint8_t j = (uint8_t)random(i + 1);
        uint8_t tmp        = s_actionDeck[i];
        s_actionDeck[i]    = s_actionDeck[j];
        s_actionDeck[j]    = tmp;
    }
    s_deckPos = 0;
}

static uint8_t pickActionTrack() {
    if (s_deckPos >= ACTION_DECK_SIZE) shuffleActionDeck();

    // Anti-repeat: track 2 appears twice; swap with the following card if it would repeat
    if (s_actionDeck[s_deckPos] == s_lastActionTrack && s_deckPos + 1 < ACTION_DECK_SIZE) {
        uint8_t tmp                  = s_actionDeck[s_deckPos];
        s_actionDeck[s_deckPos]      = s_actionDeck[s_deckPos + 1];
        s_actionDeck[s_deckPos + 1]  = tmp;
    }

    uint8_t track      = s_actionDeck[s_deckPos++];
    s_lastActionTrack  = track;
    return track;
}

// OTA removed 2026-10-01 (Joe): one less thing that can take the radio off the
// fleet channel or stop the audio while we hunt the silent-tree fault. The only
// flash path is USB through yard-bridge-pi. CMD_OTA_MODE is logged + ignored.

// =============================================================================
// Status lines (black box). Every minute one compact line; `full` adds the
// follower / PIR / radio-TX / MP3-counter block (every 5 min, and `status`).
// =============================================================================

static SerialLineReader  g_serial;      // noise-proof bench input (NodeDiag.h)
static RxFrameLog        g_rxLog;       // acted-on radio frames, once per change
static LoopGap           g_loopGap;     // longest loop() gap
static selfheal::Monitor g_heal;        // self-heal reboot decision (SelfHeal.h)

static void healStatus(uint32_t now, char* out, size_t n) {
    const uint8_t boots = healBoots();
    if (!g_heal.enabled())                 snprintf(out, n, "OFF");
    else if (g_heal.pending() != selfheal::Why::NONE)
        snprintf(out, n, "%s/%lus%s", selfheal::whyName(g_heal.pending()),
                 (unsigned long)(g_heal.pendingForMs(now) / 1000), g_heal.benchHeld(now) ? "/bench" : "");
    else                                   snprintf(out, n, "ok");
    const size_t l = strlen(out);
    if (boots && l < n) snprintf(out + l, n - l, " healBoots=%u", (unsigned)boots);
}

static void printStatus(uint32_t now, bool full) {
    static const char* const kState[] = { "AMBIENT", "ACTION", "THEME", "IDLE" };
    const uint32_t lastRx = mp3GetLastActivityMs();
    const EspNowStats es = espnowStats();
    const uint32_t scAgo = es.lastScarecrowMs ? (now - es.lastScarecrowMs) / 1000 : 0;
    char heal[48]; healStatus(now, heal, sizeof(heal));
    printTs(); Log.printf("[bb] up=%lum st=%s trk=%u mp3rx=%s%lus heal=%s heap=%u/%u loopMax=%lums slow=%lu | "
                          "rx=%lu q=%lu rep=%lu badLen=%lu badSeal=%lu notUs=%lu drop=%lu sc=%s%lus ch=%u txFail=%lu | pir=%s uartJunk=%lu/%lu\n",
                          (unsigned long)(now / 60000), kState[g_playState], (unsigned)mp3CurrentTrack(),
                          lastRx ? "" : "never/",
                          (unsigned long)(max((int32_t)0, (int32_t)(now - (lastRx ? lastRx : g_bootMs))) / 1000),
                          heal, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                          (unsigned long)g_loopGap.maxMs, (unsigned long)g_loopGap.slow,
                          (unsigned long)es.rxAll, (unsigned long)es.rxQueued, (unsigned long)g_rxLog.repeats,
                          (unsigned long)es.rxBadLen, (unsigned long)es.rxBadSeal, (unsigned long)es.rxNotUs,
                          (unsigned long)es.qDropped, es.lastScarecrowMs ? "" : "never/", (unsigned long)scAgo,
                          (unsigned)es.channel, (unsigned long)es.txFail, pirEdge().closed ? "CLOSED" : "open",
                          (unsigned long)g_serial.junkMin, (unsigned long)g_serial.junk);
    g_serial.junkMin = 0;
    g_loopGap.maxMs = 0;
    if (!full) return;

    const showfollow::Stats& st = g_follow.stats();
    printTs(); Log.printf("[show] %s theme=%u run=%lus leader=%s | plays=%u repl=%u dup=%u "
                          "earlyEOT=%u earlyOFF=%u deadline=%u unconf=%u lenWarn=%u playFail=%u badSender=%u\n",
                          showfollow::Follower::phaseName(g_follow.phase()), (unsigned)g_follow.theme(),
                          (unsigned long)(g_follow.runElapsed(now) / 1000),
                          g_follow.leaderSilent(now) ? "SILENT" : "ok",
                          st.plays, st.replaces, st.dupIgnored, st.earlyEotIgnored,
                          st.earlyOffIgnored, st.deadlineHits, st.themeUnconfirmed,
                          st.durationWarns, st.playFails, st.badSender);
    const pirlogic::PirGate& g = pirGate();
    const uint32_t left = g.leftMs(now);
    printTs(); Log.printf("[pir] radio=%s motionSent=%lu motionFail=%lu txOk=%lu txFail=%lu chRelock=%lu | "
                          "gate=%s%lus line=%s\n",
                          es.ready ? "up" : "DOWN", (unsigned long)es.motionSent, (unsigned long)es.motionFail,
                          (unsigned long)es.txOk, (unsigned long)es.txFail, (unsigned long)es.chRelocks,
                          left == UINT32_MAX ? "busy" : left ? "cooldown " : "ready ",
                          (unsigned long)(left == UINT32_MAX ? 0 : left / 1000),
                          pirEdge().closed ? "CLOSED" : "open");
    const SafeYX5300Stats& m = mp3Stats();
    printTs(); Log.printf("[mp3] track=%u plays=%u retry=%u startFail=%u stopResend=%u defuse=%u lenStop=%u "
                          "modStop=%u stale=%u fileErr=%u modErr=%u reset=%u silent=%u loopRecover=%u loopWrap=%u idx=%u echo=%u overdue=%u clean=%u\n",
                          (unsigned)mp3CurrentTrack(), m.plays, m.startRetries, m.startFails, m.stopResends, m.defuses,
                          m.lengthStops, m.moduleStops, m.staleEnds, m.fileErrors, m.moduleErrors, m.moduleResets,
                          m.silentWarns, m.loopRecovers, m.loopWraps, m.indexMismatch, m.echoes, m.loopOverdue, m.loopCleans);
    if (es.rxBadLen || es.rxBadSeal) {
        printTs(); Log.printf("[rx] last junk frame: len=%d from %02X:%02X:%02X:%02X:%02X:%02X (not a fleet Msg, dropped)\n",
                              es.lastBadLen, es.lastBadMac[0], es.lastBadMac[1], es.lastBadMac[2],
                              es.lastBadMac[3], es.lastBadMac[4], es.lastBadMac[5]);
    }
}

// ── Bench-only commands over USB serial ──────────────────────────────────────
// Show-cue simulation (2026-09-24, show-sync-orchestrator): feeds synthetic
// Scarecrow frames through the REAL onEspNowCmd() → ShowFollower path, so a
// bench run over the bridge Pi exercises exactly what a live show does.
//   visit corn|rotld|mutant — NEW session: SET_THEME + SHOW_NARRATOR (PRE-SHOW)
//   corn / rotld / mutant   — SET_THEME only (same session; mid-run = replace)
//   active                  — SHOW_ACTIVE          → theme track starts/replaces
//   off / idle              — SHOW_OFF / SHOW_IDLE → ends the run only after 3:00
//   hb                      — CMD_HEARTBEAT (liveness)
//   fake <cmd>              — same frame but from a non-Scarecrow sender (must be ignored)
//   t <n> / stop            — play track n (1 ambient, 2-6 action, 7-9 theme) / stop (ambient back in 5 s)
//   pir / piroff / piron    — simulate a trip / quiet the PIR / back on
//   pircool <s>             — PIR cooldown in seconds (default 120, until reboot)
//   log                     — dump the whole black-box log (shared/BlackBox), oldest first
//   log tail [KB]           — last KB of it (default 16)
//   log stat / log mark <text> / log clear yes
//   status                  — print the full status block now
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
    else return false;
    return true;
}

static bool startTrack(uint8_t track, uint32_t now);   // defined above loop()
static bool s_benchPir   = false;  // one simulated PIR trip (skips warmup/piroff, not the cooldown)
static bool s_pirEnabled = true;   // bench: `piroff` quiets PIR actions
static bool s_pirTrip    = false;  // a real trip, taken by the ambient branch in loop()

// Input is noise-proof (NodeDiag.h SerialLineReader: UART0 RX floats with no
// USB host). Any real command holds self-heal for 10 min (a person is here).
static void checkSerialTestCommand() {
    String s_line;
    while (g_serial.poll(s_line)) {
            if (s_line.length()) g_heal.benchActivity(millis());
            if (benchInjectCue(s_line)) {
                // handled
            } else if (s_line.startsWith("t ")) {
                // Play any track (1 = ambient loop, 2-6 action, 7-9 theme) outside
                // the show; it returns to the ambient loop when it ends.
                int t = s_line.substring(2).toInt();
                if (t < TRACK_AMBIENT || t > TRACK_THEME_MPUMP) { printTs(); Log.println("[bench] t 1..9"); }
                else {
                    g_follow.abort();
                    printTs(); Log.printf("[bench] Manual play track %d\n", t);
                    g_playState = startTrack((uint8_t)t, millis())
                                  ? (t == TRACK_AMBIENT ? PLAY_AMBIENT : PLAY_ACTION) : PLAY_IDLE;
                }
            } else if (s_line == "stop") {
                printTs(); Log.println("[bench] Manual stop — ambient resumes in 5 s");
                g_follow.abort();
                mp3Stop("bench stop");
                g_playState        = PLAY_IDLE;
                g_ambientRetryAtMs = millis() + 5000;
            } else if (s_line == "pir") {
                printTs(); Log.println("[bench] Simulated PIR trip (acts only while the ambient plays; cooldown applies)");
                s_benchPir = true;
            } else if (s_line.startsWith("pircool ")) {
                pirGate().cooldownMs = (uint32_t)s_line.substring(8).toInt() * 1000UL;
                printTs(); Log.printf("[bench] PIR cooldown %lu s (until reboot; applies from the next action track's end)\n",
                                         (unsigned long)(pirGate().cooldownMs / 1000));
            } else if (s_line == "piroff" || s_line == "piron") {
                s_pirEnabled = (s_line == "piron");
                printTs(); Log.printf("[bench] PIR %s\n", s_pirEnabled ? "ENABLED" : "DISABLED (until reboot or piron)");
            } else if (Log.command(s_line)) {
                // log / log tail / log stat / log mark / log clear yes / bbtest
            } else if (s_line == "heal off" || s_line == "heal on") {
                g_heal.setEnabled(s_line == "heal on");
                printTs(); Log.printf("[heal] self-heal %s\n", g_heal.enabled() ? "ON" : "OFF (until reboot or heal on)");
            } else if (s_line == "status") {
                printStatus(millis(), true);
            } else if (s_line == "mp3probe") {
                mp3Probe();
            } else {
                g_serial.unknown(s_line);
            }
    }
}

// Apply one ShowFollower answer. Safe from the ESP-NOW drain (sets intent only;
// the blocking mp3 call runs once in loop()) and from loop() itself.
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
        g_playState       = PLAY_THEME;                // pauses ambient + PIR
    } else if (o.act == showfollow::Act::TO_IDLE) {
        g_playState       = PLAY_IDLE;                 // ambient restarts next tick (its PLAY stops the theme)
        g_ambientRetryAtMs = 0;
    }
}

// ── ESP-NOW command handler (called from espnowTick, main-loop context) ───────

// Stale-visit guard (2026-09-27, bridge log). The Scarecrow answers any
// CMD_NODE_ONLINE (tree/Pumpkin Men boot, and Pumpkin Men every 5 min) with a
// theme burst incl. SHOW_STATE(NARRATOR) for its LAST latched session, even
// when no show is running. The tree then sat in VISIT (action tracks held) 24/7:
// the 10 min visit timeout was followed by the next resync burst within ms.
// A visit that timed out without SHOW_ACTIVE is dead: its session never starts
// a visit again. A real new show always brings a new session. Tree-side only.
static uint32_t s_narratorSession = 0;    // session of the last NARRATOR passed on
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
            if (s_deadDropped++ % 30 == 0) {           // a resync burst is 3 frames: log ~every 10th burst
                printTs(); Log.printf("[show] NARRATOR for dead session 0x%08lX ignored (x%lu)\n",
                                         (unsigned long)msg.session_id, (unsigned long)s_deadDropped);
            }
            return;
        }
        // The Scarecrow's answer to our own NODE_ONLINE (boot) is a resync, not a
        // visit starting: keep the theme it carries, but don't hold action tracks
        // for 10 min on its stale session. A real SHOW_ACTIVE still plays the theme.
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
        case CMD_STOP:                                 // emergency: end any show run, back to ambient
            g_follow.abort();
            g_pendingMp3Op = MP3_OP_STOP;
            g_playState    = PLAY_IDLE;
            printTs(); Log.printf("[main] STOP from node 0x%02X\n", msg.sender);
            break;

        case CMD_OTA_MODE:                             // OTA removed 2026-10-01: logged, ignored
            printTs(); Log.printf("[main] OTA request from node 0x%02X ignored (no OTA on the tree)\n", msg.sender);
            break;

        default:
            break;   // SHOW_LOCK / UNLOCK / TRIGGER / DUCK: Main Switch retired, ignored
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
    Log.begin("SPOOKY TREE", resetReasonName(), r == ESP_RST_POWERON);
    SafeYX5300::setLog(Log);
    const uint8_t healCount = healBootsBegin();
    printTs(); Log.println("[main] Spooky Tree booting...");
    g_bootCount++;
    printTs(); Log.printf("[main] Boot count: %u (RTC), black-box boot #%u\n", (unsigned)g_bootCount, (unsigned)Log.bootNo());
    // Why we booted: BROWNOUT here = the tree's supply sagged (2026-09-29: 22 AWG
    // feed swapped for 16 AWG after the tree went silent through power cycles).
    printTs(); Log.printf("[main] Reset reason: %s (%d)%s\n", resetReasonName(), (int)r,
                          r == ESP_RST_BROWNOUT ? "  <-- SUPPLY SAG: check the tree's power feed" :
                          (r == ESP_RST_TASK_WDT || r == ESP_RST_PANIC || r == ESP_RST_INT_WDT)
                              ? "  <-- firmware hang/crash recovered" : "");
    if (healBootThisBoot()) {
        printTs(); Log.printf("[heal] this boot is self-heal reboot %u in a row (see the HEAL REBOOT line above)\n",
                              (unsigned)healCount);
    }

    verifyNodeMac();            // eFuse MAC: no radio needed
    randomSeed(esp_random());   // seed for the action-track deck

    pirInit();

    // Settle, then MP3, then radio (see BOOT_SETTLE_*).
    const bool cold = r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT || r == ESP_RST_UNKNOWN;
    const uint32_t settleMs = cold ? BOOT_SETTLE_COLD_MS : BOOT_SETTLE_WARM_MS;
    printTs(); Log.printf("[main] %s boot: settling %lu s before the MP3 module and the radio\n",
                          cold ? "Cold" : "Warm", (unsigned long)(settleMs / 1000));
    Log.sync();
    // The MP3 has its own supply and can boot (and auto-play a card file) while we settle:
    // STOP it now and every 2 s through the settle, so nothing plays until mp3Init() (Joe 10-26).
    const uint32_t settleEnd = millis() + settleMs;
    while ((int32_t)(settleEnd - millis()) > 0) {
        mp3Quiet();
        const uint32_t left = settleEnd - millis();
        delay(left > 2000 ? 2000 : left);
    }
    printTs(); Log.println("[mp3] held quiet through the settle (STOP every 2 s)");
    mp3Init();    // shared SafeYX5300: boot dwell + warm handshake (~8 s at 500 ms pacing)
    if (mp3GetLastActivityMs()) { printTs(); Log.println("[main] MP3 module answering"); }
    else { printTs(); Log.println("[main] WARN MP3 module NOT answering after the handshake (main power? wiring?) — retries + self-heal will follow"); }

    espnowInit();               // radio last: STA mode + channel lock + ESP-NOW
    espnowSetCallback(onEspNowCmd);
    // No WiFi (Joe 10-26 14:30): the tree's own WiFi/MQTT link (ha_health.*, removed) is OFF — ESP-NOW only, as before
    // 10-26. Its draw on a supply that browned out (logs/tree/observations.md 14:16) isn't worth the HA sensor.

    // The PIR needs time to settle after power-on (the alarm PIR shares the tree's
    // power, so it warms up with the ESP); ignore trips until then
    g_pirReadyMs = millis() + 60000;
    printTs(); Log.printf("[main] PIR warmup — triggers enabled in 60s (at t=%ums)\n",
                  (unsigned)g_pirReadyMs);

    // Loop watchdog (2026-09-29, "never stuck"): if loop() stops coming round
    // for LOOP_WDT_S the chip reboots and the ambient restarts from boot. The
    // longest legit block is a failed start (3 attempts incl. a warm handshake,
    // ~15 s); a full `log` dump feeds it per chunk.
    const esp_err_t wdtErr = esp_task_wdt_init(LOOP_WDT_S, true);
    enableLoopWDT();
    printTs(); Log.printf("[main] Loop watchdog %s (%d s, err=%d)\n",
                             wdtErr == ESP_OK ? "armed" : "WARN init failed", LOOP_WDT_S, (int)wdtErr);

    g_bootMs = millis();
    espnowSendNodeOnline();
    printTs(); Log.printf("[main] Boot complete in %lu s. Idle mode (ambient + PIR); following the Scarecrow\n",
                          (unsigned long)(millis() / 1000));
}

// =============================================================================
// loop
// =============================================================================

// Start a track and record the outcome. Theme failures go to the follower;
// ambient/action failures fall back to ambient with a back-off.
// Ambient uses the module's gapless repeat (SafeYX5300 P9, bench-proven on the
// Pumpkin Men idle loop 2026-09-26) instead of STOP/PLAY every 6:21.
static bool startTrack(uint8_t track, uint32_t now) {
    bool ok = (track == TRACK_AMBIENT) ? mp3PlayLoop(track) : mp3PlayTrack(track);
    if (!ok) {
        mp3ClearFinished();                            // the failure is handled here, not as an end
        printTs(); Log.printf("[main] WARN track %u did not start (%s)\n",
                                 (unsigned)track, SafeYX5300::endName(mp3EndReason()));
    }
    (void)now;
    return ok;
}

// ── Health state (2026-10-26) — black box only: logged on every change. The HA report over the tree's
// own WiFi was removed the same day (no WiFi on this supply; HA never switches the yard stake).
static void healthTick() {
    static uint8_t  s_last = 0xFF;
    static uint32_t s_sentAt = 0;
    const uint32_t now = millis();
    uint8_t h;
    if (mp3CurrentTrack())                         h = HEALTH_PLAYING;
    else if (mp3Echoing())                         h = HEALTH_ECHO;
    else if (now - g_bootMs < 60000UL)             h = HEALTH_BOOTING;
    else if (now - mp3GetLastActivityMs() > 60000UL) h = HEALTH_NO_MODULE;
    else                                           h = HEALTH_NOT_PLAYING;
    const bool busy = g_follow.phase() != showfollow::Phase::IDLE;
    if (h == s_last) return;
    if (s_sentAt && now - s_sentAt < 5000UL) return;              // flapping: at most one line per 5 s
    static const char* const kName[] = {"BOOTING", "PLAYING", "NOT_PLAYING", "NO_MODULE", "ECHO"};
    const SafeYX5300Stats& m = mp3Stats();
    char extra[160];
    snprintf(extra, sizeof(extra), "\"plays\":%u,\"start_fails\":%u,\"echoes\":%u,\"heal_boots\":%u",
             m.plays, m.startFails, m.echoes, (unsigned)healBoots());
    s_sentAt = now;
    s_last = h;
    printTs(); Log.printf("[health] %s%s %s\n", kName[h], busy ? " (show busy)" : "", extra);
}

// ── Self-heal (SelfHeal.h) ────────────────────────────────────────────────────
// Safe = the tree is only doing its own idle thing: no theme run, no visit, no
// action track, nothing pending. A reboot then costs ~15 s of ambient, nothing
// a visitor would connect to the show.
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
        healBootsMark();
        Log.sync();
        delay(100);
        ESP.restart();
    }
    // A long healthy run clears the in-a-row count (SelfHeal.h reboot-loop guard).
    if (healBoots() && w == selfheal::Why::NONE &&
        (int32_t)(now - g_bootMs) >= (int32_t)g_heal.config().healthyMs) {
        printTs(); Log.printf("[heal] healthy %lu min — self-heal reboot count %u cleared\n",
                              (unsigned long)(g_heal.config().healthyMs / 60000), (unsigned)healBoots());
        healBootsClear();
    }
}

void loop() {
    const uint32_t now = millis();

    checkSerialTestCommand();  // BENCH TEST ONLY — see comment above its definition

    g_loopGap.tick(now, (uint8_t)g_playState, mp3CurrentTrack());   // WARNs a >= 5 s block
    Log.tick();

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
        if (op == MP3_OP_STOP) {
            mp3Stop("STOP command");
        } else if (op == MP3_OP_PLAY) {
            if (!startTrack(g_pendingMp3Track, now) && g_follow.isPlaying())
                applyFollower(g_follow.onPlayFailed(millis()));
        }
    }

    espnowHeartbeatTick();
    mp3Tick();
    {
        static const char* const kState[] = { "AMBIENT", "ACTION", "THEME", "IDLE" };

        // PIR edge tracking runs in every state (pir_logic.h). A trip is acted on
        // only while the ambient plays; one that lands mid-theme/action is dropped
        // here, logged, and does not start the cooldown.
        const uint32_t nowP = millis();
        const pirlogic::Ev ev = pirTick(nowP);
        if (ev == pirlogic::Ev::TRIP) {
            if (g_playState == PLAY_AMBIENT) s_pirTrip = true;
            else { printTs(); Log.printf("[pir] trip ignored (%s playing)\n", kState[g_playState]); }
        } else if (ev == pirlogic::Ev::RELEASE) {
            printTs(); Log.printf("[pir] released after %lu ms\n", (unsigned long)pirEdge().heldMs);
        } else if (ev == pirlogic::Ev::FAULT) {
            printTs(); Log.println("[pir] FAULT — relay closed 5 min (stuck relay / sensor?); no trips until it opens");
        }
        // Cooldown starts once the reaction is over, however the action track ended
        // (FILE_END, 1-min cap, a theme taking over, watchdog, bench stop).
        if (g_playState != PLAY_ACTION) pirGate().reactionDone(nowP);
    }

    // ── Show follower: the theme run's end + fail-safe timers ─────────────────
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

    // ── Idle mode: ambient loop + PIR action tracks ───────────────────────────
    if (g_playState == PLAY_IDLE) {
        if ((int32_t)(now - g_ambientRetryAtMs) >= 0) {
            if (startTrack(TRACK_AMBIENT, now)) {
                g_playState = PLAY_AMBIENT;
                g_heal.idleStartOk();
            } else {
                g_ambientRetryAtMs = now + AMBIENT_RETRY_BACKOFF_MS;
                g_heal.idleStartFailed();
            }
        }
    }

    if (g_playState == PLAY_AMBIENT) {
        // Ambient repeats on the module; it only "finishes" if the module stopped
        // it / reset / failed -> back to idle, which restarts it at once.
        if (mp3IsFinished()) {
            printTs(); Log.printf("[main] ambient loop ended (%s) — restarting\n", SafeYX5300::endName(mp3EndReason()));
            mp3ClearFinished();
            g_heal.idleBedDied(now);                   // repeated deaths = LOOP_CHURN self-heal
            g_playState = PLAY_IDLE;                   // restarts via the idle branch next tick
            s_pirTrip   = false;                       // no stale trip acted on after the restart
            return;
        }

        // PIR: a fresh relay close, past the cooldown -> report the motion to the
        // fleet (CMD_PROP_MOTION) and play an action track (held while a visit's
        // narrators are talking). Every dropped trip is logged with its reason.
        if (s_pirTrip || s_benchPir) {
            const bool bench = s_benchPir;
            s_pirTrip = s_benchPir = false;
            pirlogic::PirGate& gate = pirGate();
            const char* skip = nullptr;
            if      (!bench && (int32_t)(now - g_pirReadyMs) < 0) skip = "warmup";
            else if (!bench && !s_pirEnabled)                      skip = "piroff";
            else if (!gate.ready(now))                             skip = "cooldown";
            if (skip) {
                printTs(); Log.printf("[pir] trip ignored (%s", skip);
                if (gate.leftMs(now) && gate.leftMs(now) != UINT32_MAX)
                    Log.printf(", %lu s left", (unsigned long)(gate.leftMs(now) / 1000));
                Log.println(")");
            } else {
                gate.acted();
                // The Scarecrow report has its own, longer cooldown (Joe: no aside
                // every 2 min), counted from the last report sent.
                static pirlogic::PirGate s_motionGate;
                s_motionGate.cooldownMs = PROP_MOTION_COOLDOWN_MS;
                if (!s_motionGate.ready(now)) {
                    printTs(); Log.printf("[pir] PROP_MOTION not sent (Scarecrow cooldown, %lu s left)\n",
                                             (unsigned long)(s_motionGate.leftMs(now) / 1000));
                } else {
                    if (g_follow.leaderSilent(now)) {
                        printTs(); Log.println("[pir] WARN Scarecrow not heard for 35 s+ — PROP_MOTION sent anyway, may not be voiced");
                    }
                    espnowSendPropMotion();            // logs its own result
                    s_motionGate.acted(); s_motionGate.reactionDone(now);
                }
                if (g_follow.inVisit()) {
                    printTs(); Log.printf("[pir] MOTION%s — action track held (narrators talking)\n",
                                             bench ? " (bench)" : "");
                } else {
                    uint8_t actionTrack = pickActionTrack();
                    printTs(); Log.printf("[pir] MOTION%s — action track %u\n",
                                             bench ? " (bench)" : "", (unsigned)actionTrack);
                    g_playState = startTrack(actionTrack, now) ? PLAY_ACTION : PLAY_IDLE;
                }
            }
        }
    }

    if (g_playState == PLAY_ACTION && mp3IsFinished()) {
        mp3ClearFinished();                            // action done (FILE_END / 1-min cap) → ambient
        g_playState = PLAY_IDLE;
    }

    selfHealTick();
    healthTick();

    delay(10);
}
