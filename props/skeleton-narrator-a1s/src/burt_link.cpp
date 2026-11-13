#include "burt_link.h"
#include "NarratorBanter.h"
#include "pools.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include "wifi_credentials.h"

static uint16_t s_seq       = 0;
static bool     s_wifiUp    = false;
static bool     s_espnowUp  = false;
static char     s_ip[16]    = "0.0.0.0";

// latched show context
static ShowTheme s_theme    = THEME_CORN;
static ShowMode  s_tone     = MODE_SCARY;
static uint32_t  s_session  = 0;
static bool      s_mainshow = false;

// inbound cue (set in RX callback, consumed in loop)
static volatile bool     s_cuePending = false;
static volatile uint32_t s_cueSession = 0;
static volatile uint8_t  s_cueTheme   = 0;
static volatile uint8_t  s_cueTone    = 0;
static volatile bool     s_cueMain    = false;
static volatile bool     s_cueHold    = false;
static volatile uint32_t s_cueBits    = 0;
// Banter-turn dedupe: by exact (session, turn), NOT seq +/-1 -- two back-to-
// back turns can carry neighbouring seq numbers and would be dropped as dups.
static uint32_t s_lastTurnSession = 0;
static uint32_t s_lastTurnBits    = 0;

// Dedupe key for CMD_NARRATOR_2_START. NOT (session_id, phase) — session_id
// is constant for the whole cycle and `phase` (mainshow) stays false through
// preshow AND every HOLD retrigger, so that pair can't tell three separate
// HOLD events apart from each other or from the original preshow cue (found
// 2026-09-11, TASK-a1s-hold-segment.md Part B). `seq` is per-Msg and strictly
// increasing across the whole session; fleet_cue_burt() builds exactly two
// Msgs per call (broadcast copy, then the unicast copy at seq+1), back to
// back with nothing else incrementing seq in between — so "seq == last
// accepted, or exactly one more" reliably identifies the 3x-broadcast +
// unicast copies of the SAME cue, and anything else is a genuinely new one.
static uint16_t s_lastAcceptedSeq      = 0xFFFF;
static bool     s_haveAcceptedSeq      = false;

// native send-callback capture for the Scarecrow unicast
static volatile bool s_ackDone = false;
static volatile bool s_ackOk   = false;

// diagnostics
static uint32_t s_bcastRx = 0, s_cueRx = 0, s_ackOkCount = 0, s_ackFailCount = 0;

static void ts() {
    uint32_t ms = millis();
    Serial.printf("[%02u:%02u.%03u] ", ms / 60000, (ms % 60000) / 1000, ms % 1000);
}

static Msg makeMsg(Command cmd, uint8_t target = NODE_BROADCAST) {
    Msg m = {};
    m.command    = (uint8_t)cmd;
    m.sender     = (uint8_t)NODE_SKELETON_A1S;
    m.target     = target;
    m.session_id = s_session;
    m.seq        = s_seq++;
    msg_seal(m);
    return m;
}

static void onSend(const uint8_t* mac, esp_now_send_status_t status) {
    if (mac && memcmp(mac, MAC_SCARECROW_A1S, 6) == 0) {
        s_ackOk   = (status == ESP_NOW_SEND_SUCCESS);
        s_ackDone = true;
    }
}

static void onRecvCommon(const uint8_t* /*mac*/, const uint8_t* data, int len) {
    if (len != (int)sizeof(Msg)) return;
    Msg m;
    memcpy(&m, data, sizeof(Msg));
    if (!msg_valid(m)) return;

    s_bcastRx++;
    switch ((Command)m.command) {
        case CMD_SET_THEME:   s_theme = (ShowTheme)m.payload_a; break;
        case CMD_SET_MODE:    s_tone  = (ShowMode)m.payload_a;  break;
        case CMD_START_SCENE:
            if (m.session_id) { s_session = m.session_id; }
            s_mainshow = true;
            break;
        case CMD_SHOW_STATE:
            // SHOW_NARRATOR = a new PRE-SHOW is starting (Scarecrow's theme
            // burst), so the previous main-show latch can't still apply.
            // 2026-09-24: this used to self-heal every 10s via Scarecrow's
            // SHOW_IDLE heartbeat; that heartbeat is now CMD_HEARTBEAT, so a
            // missed reset burst is covered here instead.
            if (m.payload_a == SHOW_IDLE || m.payload_a == SHOW_OFF ||
                m.payload_a == SHOW_NARRATOR) s_mainshow = false;
            break;
        case CMD_NARRATOR_2_START: {
            // Burt's cue — from the broadcast burst OR the Scarecrow unicast copy.
            // payload_a = theme; payload_b low byte = tone, upper byte = HOLD flag
            // (a1s doc §3.5, TASK-a1s-hold-segment.md Part B). Dedupe on `seq`
            // (+/-1 to tolerate the broadcast/unicast pair landing either order),
            // not (session_id, phase) — see the comment on s_lastAcceptedSeq above
            // for why that pair can't tell separate HOLD retriggers apart.
            if (banter_is_turn(m.payload_b)) {
                uint32_t turn = m.payload_b & NARRATOR2_TURN_MASK;
                if (m.session_id == s_lastTurnSession && turn == s_lastTurnBits) return;  // dup copy
                s_lastTurnSession = m.session_id;
                s_lastTurnBits    = turn;
                s_lastAcceptedSeq = m.seq;
                s_haveAcceptedSeq = true;
            } else {
            bool isDup = s_haveAcceptedSeq &&
                         (m.seq == s_lastAcceptedSeq ||
                          m.seq == (uint16_t)(s_lastAcceptedSeq + 1) ||
                          m.seq == (uint16_t)(s_lastAcceptedSeq - 1));
            if (isDup) return;
            s_lastAcceptedSeq = m.seq;
            s_haveAcceptedSeq = true;
            }

            s_session    = m.session_id;
            s_cueSession = m.session_id;
            s_cueTheme   = (uint8_t)m.payload_a;
            s_cueTone    = (uint8_t)(m.payload_b & 0xFF);
            s_cueHold    = (m.payload_b & 0x100) != 0;
            s_cueBits    = m.payload_b;
            s_cueMain    = s_mainshow;   // latched phase — not carried on the wire
            s_cuePending = true;
            s_cueRx++;
            break;
        }
        default: break;
    }
}

#if ESP_IDF_VERSION_MAJOR >= 5
static void onRecvV5(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    onRecvCommon(info->src_addr, data, len);
}
#else
static void onRecvLegacy(const uint8_t* mac, const uint8_t* data, int len) {
    onRecvCommon(mac, data, len);
}
#endif

static void wifiConnect() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    ts(); Serial.printf("[burt] WiFi assoc to \"%s\"", WIFI_SSID);
    uint32_t deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) { delay(250); Serial.print('.'); }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        s_wifiUp = true;
        strncpy(s_ip, WiFi.localIP().toString().c_str(), sizeof(s_ip) - 1);
        ts(); Serial.printf("[burt] WiFi up: IP %s  RSSI %d  channel %d\n",
                            s_ip, WiFi.RSSI(), WiFi.channel());
        if (WiFi.channel() != WIFI_CHANNEL) {
            ts(); Serial.printf("[burt] !! channel drift: on %d, want %d\n", WiFi.channel(), WIFI_CHANNEL);
        }
    } else {
        s_wifiUp = false;
        ts(); Serial.println(F("[burt] WiFi failed — ESP-NOW only, locking ch 11"));
        esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    }
}

void burt_begin() {
    wifiConnect();

    if (esp_now_init() != ESP_OK) { ts(); Serial.println(F("[burt] esp_now_init FAILED")); return; }
    esp_now_register_send_cb(onSend);
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_now_register_recv_cb(onRecvV5);
#else
    esp_now_register_recv_cb(onRecvLegacy);
#endif

    esp_now_peer_info_t bc = {};
    memcpy(bc.peer_addr, MAC_BROADCAST, 6);
    bc.channel = 0; bc.encrypt = false;
    esp_now_add_peer(&bc);

    esp_now_peer_info_t sc = {};
    memcpy(sc.peer_addr, MAC_SCARECROW_A1S, 6);
    sc.channel = 0; sc.encrypt = false;
    esp_now_add_peer(&sc);

    s_espnowUp = true;
    ts(); Serial.printf("[burt] ESP-NOW up (broadcast + Scarecrow %02X:%02X:%02X:%02X:%02X:%02X)\n",
                        MAC_SCARECROW_A1S[0], MAC_SCARECROW_A1S[1], MAC_SCARECROW_A1S[2],
                        MAC_SCARECROW_A1S[3], MAC_SCARECROW_A1S[4], MAC_SCARECROW_A1S[5]);
}

// 2026-09-25: WiFi is retried for as long as it is down (same fix as the
// Scarecrow's fleet_tick). Before this, a failed boot connect or one failed
// reconnect left s_wifiUp false forever: ESP-NOW still worked but no OTA and
// no middle/<session>_bu.wav fetch. Non-blocking; drop/retry/recovery logged.
#define WIFI_RETRY_MS 10000
void burt_tick() {
    static uint32_t last = 0;
    static uint32_t lastRetry = 0;
    static uint32_t retries   = 0;
    if (millis() - last < 3000) return;
    last = millis();
    if (WiFi.status() != WL_CONNECTED) {
        if (s_wifiUp) {
            s_wifiUp = false;
            strncpy(s_ip, "0.0.0.0", sizeof(s_ip) - 1);
            ts(); Serial.println(F("[burt] WiFi dropped — retrying every 10s"));
        }
        if (millis() - lastRetry >= WIFI_RETRY_MS) {
            lastRetry = millis();
            retries++;
            ts(); Serial.printf("[burt] WiFi retry #%lu\n", (unsigned long)retries);
            WiFi.disconnect(false, false);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
        return;
    }
    if (!s_wifiUp) {
        s_wifiUp = true;
        strncpy(s_ip, WiFi.localIP().toString().c_str(), sizeof(s_ip) - 1);
        ts(); Serial.printf("[burt] WiFi RECOVERED after %lu retries: IP %s  RSSI %d  channel %d\n",
                            (unsigned long)retries, s_ip, WiFi.RSSI(), WiFi.channel());
        retries = 0;
    }
}

bool burt_wifi_up()      { return s_wifiUp; }
int  burt_wifi_channel() { return s_wifiUp ? WiFi.channel() : -1; }
const char* burt_ip()    { return s_ip; }

bool burt_take_cue(BurtCue& out) {
    if (!s_cuePending) return false;
    noInterrupts();
    out.pending  = true;
    out.session  = s_cueSession;
    out.theme    = (ShowTheme)s_cueTheme;
    out.tone     = (ShowMode)s_cueTone;
    out.mainshow = s_cueMain;
    out.hold     = s_cueHold;
    out.turnBits = s_cueBits;
    out.banter   = banter_is_turn(s_cueBits);
    s_cuePending = false;
    interrupts();
    ts(); Serial.printf("[burt] <- CUE  session=0x%08lX theme=%d tone=%d %s\n",
                        (unsigned long)out.session, out.theme, out.tone,
                        out.hold ? "(HOLD)" : out.mainshow ? "(main-show OUTRO)" : "(pre-show)");
    return true;
}

bool burt_send_starting_ack(uint32_t session) {
    if (!s_espnowUp) return false;
    Msg m = makeMsg(CMD_ACK, NODE_SCARECROW_A1S);
    m.payload_a = NODE_SKELETON_A1S;
    m.payload_b = session;
    m.flags     = ACK_FLAG_STATUS_VALID | (pools_mounted() ? ACK_FLAG_SD_OK : 0);
    msg_seal(m);

    bool ok = false;
    uint8_t attempts = 0;
    for (uint8_t i = 1; i <= 3; i++) {
        attempts = i;
        s_ackDone = false; s_ackOk = false;
        if (esp_now_send(MAC_SCARECROW_A1S, (const uint8_t*)&m, sizeof(m)) == ESP_OK) {
            uint32_t t0 = millis();
            while (!s_ackDone && (millis() - t0) < 50) delay(1);
            if (s_ackDone && s_ackOk) { ok = true; break; }
        }
        if (i < 3) delay(30);
    }
    if (ok) s_ackOkCount++; else s_ackFailCount++;
    ts(); Serial.printf("[burt] -> starting-ack to Scarecrow: %s after %u attempt(s) (diagnostic; not waited on)\n",
                        ok ? "SUCCESS" : "no confirmation", attempts);
    return ok;
}

void burt_broadcast_narrator_done(bool mainshow) {
    if (!s_espnowUp) return;
    Msg m = makeMsg(CMD_NARRATOR_2_DONE);
    m.payload_a = mainshow ? 1 : 0;
    msg_seal(m);
    for (int i = 0; i < 3; i++) { esp_now_send(MAC_BROADCAST, (const uint8_t*)&m, sizeof(m)); if (i < 2) delay(200); }
    ts(); Serial.printf("[burt] -> NARRATOR_2_DONE (3x)  mainshow=%d\n", mainshow);
}

void burt_broadcast_turn_done(uint32_t turnBits, bool played) {
    if (!s_espnowUp) return;
    Msg m = makeMsg(CMD_NARRATOR_2_DONE);
    m.payload_b = (turnBits & NARRATOR2_TURN_MASK) | (played ? NARRATOR2_DONE_PLAYED : 0);
    msg_seal(m);
    for (int i = 0; i < 3; i++) { esp_now_send(MAC_BROADCAST, (const uint8_t*)&m, sizeof(m)); if (i < 2) delay(150); }
    ts(); Serial.printf("[burt] -> DONE banter %s #%u line %u  played=%d (3x)\n",
                        BANTER_POOL_NAMES[banter_pool(turnBits)], banter_idx(turnBits),
                        banter_line(turnBits), played);
}

void burt_print_log() {
    ts(); Serial.printf("[burt] rx: frames=%lu cues=%lu | starting-ack ok=%lu fail=%lu | latched theme=%d tone=%d mainshow=%d\n",
                        (unsigned long)s_bcastRx, (unsigned long)s_cueRx,
                        (unsigned long)s_ackOkCount, (unsigned long)s_ackFailCount,
                        s_theme, s_tone, s_mainshow);
}
