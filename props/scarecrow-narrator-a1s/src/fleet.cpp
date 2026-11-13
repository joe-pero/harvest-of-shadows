#include "fleet.h"
#include "NarratorBanter.h"
#include "audio_out.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include "wifi_credentials.h"

// ── state ───────────────────────────────────────────────────────────────────
static uint32_t s_session      = 0;
static uint16_t s_seq          = 0;
static bool     s_wifiUp       = false;
static bool     s_espnowUp     = false;
static uint32_t s_lastHeartbeat = 0;
static char     s_ip[16]       = "0.0.0.0";

// native send-callback capture for the Burt unicast
static volatile bool s_burtDone = false;
static volatile bool s_burtOk   = false;

// Banter turn wait (NarratorBanter.h): the waiter arms the exact session +
// turn it expects; the rx callback only marks a DONE that matches, so a stale
// or duplicate DONE (Burt sends each 3x) can never release the next line early.
static volatile uint32_t s_treeMotionMs = 0;     // millis() of the last tree PIR report (0 = none)
static volatile uint32_t s_treeMotionRx = 0;
static volatile bool     s_turnArmed   = false;
static volatile uint32_t s_turnSession = 0;
static volatile uint32_t s_turnBits    = 0;
static volatile bool     s_turnMatched = false;
static volatile bool     s_turnPlayed  = false;

// handshake diagnostics rollup
static uint32_t s_hsCue_ok = 0, s_hsCue_fail = 0, s_hsCue_attempts = 0;
static uint32_t s_burtAckRx = 0;
static int8_t   s_burtSd    = -1;   // -1 unknown, 0 FAILED, 1 ok (Burt's CMD_ACK flags)

// set from the ESP-NOW RX callback, polled (and cleared) from main.cpp
static volatile bool s_narrator2Done  = false;
static volatile bool s_nodeOnlineSeen = false;

// HOLD flag packed into the unused upper byte of CMD_NARRATOR_2_START's
// payload_b (low byte still carries ShowMode tone, 0..2) — see fleet.h.
static const uint32_t NARRATOR2_HOLD_BIT = 0x100;

static void ts() {
    uint32_t ms = millis();
    Serial.printf("[%02u:%02u.%03u] ", ms / 60000, (ms % 60000) / 1000, ms % 1000);
}

// ── Msg factory ─────────────────────────────────────────────────────────────
static Msg makeMsg(Command cmd, uint8_t target = NODE_BROADCAST) {
    Msg m = {};
    m.command    = (uint8_t)cmd;
    m.sender     = (uint8_t)NODE_SCARECROW_A1S;
    m.target     = target;
    m.flags      = 0;
    m.session_id = s_session;
    m.seq        = s_seq++;
    m.payload_a  = 0;
    m.payload_b  = 0;
    msg_seal(m);
    return m;
}

// ── ESP-NOW callbacks ───────────────────────────────────────────────────────
static void onSend(const uint8_t* mac, esp_now_send_status_t status) {
    // only the Burt unicast cares about the native result
    if (mac && memcmp(mac, MAC_SKELETON_A1S, 6) == 0) {
        s_burtOk   = (status == ESP_NOW_SEND_SUCCESS);
        s_burtDone = true;
    }
}

static void onRecvCommon(const uint8_t* /*mac*/, const uint8_t* data, int len) {
    if (len != (int)sizeof(Msg)) return;
    Msg m;
    memcpy(&m, data, sizeof(Msg));
    if (!msg_valid(m)) return;
    // H1b handoff: Burt -> Scarecrow "starting" ack (diagnostic only)
    if (m.command == CMD_ACK && m.payload_a == NODE_SKELETON_A1S) {
        s_burtAckRx++;
        if (m.flags & ACK_FLAG_STATUS_VALID) {
            int8_t sd = (m.flags & ACK_FLAG_SD_OK) ? 1 : 0;
            if (sd == 0) { ts(); Serial.println(F("[fleet] !! Burt reports SD NOT MOUNTED -- he is playing placeholder tones")); }
            else if (s_burtSd == 0) { ts(); Serial.println(F("[fleet] Burt reports SD recovered")); }
            s_burtSd = sd;
        }
        ts(); Serial.printf("[fleet] <- Burt CMD_ACK (starting), session=0x%08lX  [total %lu]\n",
                            (unsigned long)m.payload_b, (unsigned long)s_burtAckRx);
    }
    // Burt's real completion broadcast — gates the post-narrator SHOW_ACTIVE
    // transition (main.cpp) instead of firing immediately after the cue.
    if (m.command == CMD_NARRATOR_2_DONE) {
        if (banter_is_turn(m.payload_b)) {
            // banter turn DONE: only the armed turn counts; never touches the
            // legacy s_narrator2Done flag (that would release doMainshow early)
            if (s_turnArmed && !s_turnMatched && m.session_id == s_turnSession &&
                (m.payload_b & NARRATOR2_TURN_MASK) == (s_turnBits & NARRATOR2_TURN_MASK)) {
                s_turnPlayed  = (m.payload_b & NARRATOR2_DONE_PLAYED) != 0;
                s_turnMatched = true;
            }
        } else {
            s_narrator2Done = true;
            ts(); Serial.println(F("[fleet] <- NARRATOR_2_DONE (Burt)"));
        }
    }
    // Spooky Tree PIR (CMD_PROP_MOTION, payload_a = NodeID). Sent 3x per
    // detection; only the timestamp matters, so duplicates are harmless.
    if (m.command == CMD_PROP_MOTION && m.payload_a == NODE_SPOOKY_TREE) {
        s_treeMotionMs = millis();
        if (s_treeMotionMs == 0) s_treeMotionMs = 1;
        s_treeMotionRx++;
    }
    // H4 reboot-resync: any node announcing it just came online.
    if (m.command == CMD_NODE_ONLINE) {
        s_nodeOnlineSeen = true;
        ts(); Serial.printf("[fleet] <- CMD_NODE_ONLINE from node 0x%02X\n", m.sender);
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

// ── raw send helpers ────────────────────────────────────────────────────────
static void sendBroadcastOnce(const Msg& m) {
    esp_now_send(MAC_BROADCAST, (const uint8_t*)&m, sizeof(m));
}
static void sendBroadcast3x(const Msg& m) {          // H1: 3x, ~200 ms apart
    for (int i = 0; i < 3; i++) {
        sendBroadcastOnce(m);
        if (i < 2) delay(200);
    }
}

// ── WiFi ────────────────────────────────────────────────────────────────────
static void wifiConnect() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);                 // H3: no modem-sleep — ESP-NOW RX latency
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    ts(); Serial.printf("[fleet] WiFi assoc to \"%s\"", WIFI_SSID);
    uint32_t deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) { delay(250); Serial.print('.'); }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        s_wifiUp = true;
        strncpy(s_ip, WiFi.localIP().toString().c_str(), sizeof(s_ip) - 1);
        int ch = WiFi.channel();
        ts(); Serial.printf("[fleet] WiFi up: IP %s  RSSI %d  channel %d\n", s_ip, WiFi.RSSI(), ch);
        if (ch != WIFI_CHANNEL) {
            ts(); Serial.printf("[fleet] !! CHANNEL DRIFT — associated on %d, expected %d. "
                                "ESP-NOW peers will be unreachable until this AP returns to 11.\n",
                                ch, WIFI_CHANNEL);
        }
    } else {
        s_wifiUp = false;
        ts(); Serial.println(F("[fleet] WiFi connect FAILED — leader runs ESP-NOW-only; "
                               "lock radio to ch 11 manually"));
        esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    }
}

// ── public API ──────────────────────────────────────────────────────────────
void fleet_begin(uint32_t initial_session) {
    s_session = initial_session;

    wifiConnect();

    if (esp_now_init() != ESP_OK) {
        ts(); Serial.println(F("[fleet] esp_now_init FAILED"));
        return;
    }
    esp_now_register_send_cb(onSend);
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_now_register_recv_cb(onRecvV5);
#else
    esp_now_register_recv_cb(onRecvLegacy);
#endif

    // broadcast peer — the whole fleet
    esp_now_peer_info_t bc = {};
    memcpy(bc.peer_addr, MAC_BROADCAST, 6);
    bc.channel = 0;                       // 0 = current WiFi channel
    bc.encrypt = false;
    esp_now_add_peer(&bc);

    // unicast peer — Burt (the one Tier-2 relationship)
    esp_now_peer_info_t burt = {};
    memcpy(burt.peer_addr, MAC_SKELETON_A1S, 6);
    burt.channel = 0;
    burt.encrypt = false;
    esp_now_add_peer(&burt);

    s_espnowUp = true;
    ts(); Serial.printf("[fleet] ESP-NOW up (broadcast + Burt %02X:%02X:%02X:%02X:%02X:%02X). session=0x%08lX\n",
                        MAC_SKELETON_A1S[0], MAC_SKELETON_A1S[1], MAC_SKELETON_A1S[2],
                        MAC_SKELETON_A1S[3], MAC_SKELETON_A1S[4], MAC_SKELETON_A1S[5],
                        (unsigned long)s_session);
}

// 2026-09-25: WiFi is retried for as long as it is down. Before this, a failed
// boot connect (or one failed reconnect) left s_wifiUp false and nothing ever
// tried again -> no MQTT, bridge paused cues, Scarecrow silent until a power
// cycle (seen live 2026-09-25). Retries are non-blocking (no 15s wait inside
// the loop, audio/cues keep running) and every drop/recovery is logged.
#define WIFI_RETRY_MS 10000
void fleet_tick() {
    static uint32_t lastCheck = 0;
    static uint32_t lastRetry = 0;
    static uint32_t retries   = 0;
    if (millis() - lastCheck < 3000) return;
    lastCheck = millis();
    bool connected = (WiFi.status() == WL_CONNECTED);
    if (!connected) {
        if (s_wifiUp) {
            s_wifiUp = false;
            strncpy(s_ip, "0.0.0.0", sizeof(s_ip) - 1);
            ts(); Serial.println(F("[fleet] WiFi dropped — retrying every 10s"));
        }
        if (millis() - lastRetry >= WIFI_RETRY_MS) {
            lastRetry = millis();
            retries++;
            ts(); Serial.printf("[fleet] WiFi retry #%lu\n", (unsigned long)retries);
            WiFi.disconnect(false, false);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
        return;
    }
    if (!s_wifiUp) {
        s_wifiUp = true;
        strncpy(s_ip, WiFi.localIP().toString().c_str(), sizeof(s_ip) - 1);
        ts(); Serial.printf("[fleet] WiFi RECOVERED after %lu retries: IP %s  RSSI %d  channel %d\n",
                            (unsigned long)retries, s_ip, WiFi.RSSI(), WiFi.channel());
        retries = 0;
    } else {
        int ch = WiFi.channel();
        if (ch != WIFI_CHANNEL) {
            ts(); Serial.printf("[fleet] channel drift: on %d not %d\n", ch, WIFI_CHANNEL);
        }
    }
}

bool fleet_wifi_up()      { return s_wifiUp; }
int8_t fleet_burt_sd()    { return s_burtSd; }
int  fleet_wifi_channel() { return s_wifiUp ? WiFi.channel() : -1; }
const char* fleet_ip()    { return s_ip; }
void fleet_set_session(uint32_t s) { s_session = s; }

// ── leader broadcasts ───────────────────────────────────────────────────────
void fleet_broadcast_theme_burst(ShowTheme theme, ShowMode tone) {
    if (!s_espnowUp) return;
    ts(); Serial.printf("[fleet] -> THEME burst  theme=%d tone=%d session=0x%08lX (3x each)\n",
                        theme, tone, (unsigned long)s_session);
    Msg t = makeMsg(CMD_SET_THEME);  t.payload_a = theme; msg_seal(t);
    Msg d = makeMsg(CMD_SET_MODE);   d.payload_a = tone;  msg_seal(d);
    Msg s = makeMsg(CMD_SHOW_STATE); s.payload_a = SHOW_NARRATOR; msg_seal(s);
    // one group, sent 3x, ~150-300 ms apart (H1)
    for (int i = 0; i < 3; i++) {
        sendBroadcastOnce(t); delay(15);
        sendBroadcastOnce(d); delay(15);
        sendBroadcastOnce(s);
        if (i < 2) delay(220);
    }
}

void fleet_broadcast_show_state(ShowState st) {
    if (!s_espnowUp) return;
    Msg m = makeMsg(CMD_SHOW_STATE); m.payload_a = st; msg_seal(m);
    sendBroadcast3x(m);
    ts(); Serial.printf("[fleet] -> SHOW_STATE %d (3x)\n", st);
}

void fleet_broadcast_start_scene(ShowTheme theme) {
    if (!s_espnowUp) return;
    Msg m = makeMsg(CMD_START_SCENE); m.payload_a = theme; msg_seal(m);
    sendBroadcast3x(m);
    ts(); Serial.printf("[fleet] -> START_SCENE theme=%d (3x)\n", theme);
}

void fleet_broadcast_narrator_done(uint8_t which) {
    if (!s_espnowUp) return;
    Msg m = makeMsg(which == 1 ? CMD_NARRATOR_1_DONE : CMD_NARRATOR_2_DONE);
    sendBroadcast3x(m);
    ts(); Serial.printf("[fleet] -> NARRATOR_%u_DONE (3x)\n", which);
}

void fleet_broadcast_node_online() {
    if (!s_espnowUp) return;
    Msg m = makeMsg(CMD_NODE_ONLINE);
    sendBroadcast3x(m);
    ts(); Serial.println(F("[fleet] -> CMD_NODE_ONLINE (3x)"));
}

// ── H1b: cue Burt ───────────────────────────────────────────────────────────
static FleetHandshake cueBurtRaw(ShowTheme theme, uint32_t payloadB, const char* label);

FleetHandshake fleet_cue_burt(ShowTheme theme, ShowMode tone, bool hold) {
    uint32_t payloadB = (uint32_t)tone | (hold ? NARRATOR2_HOLD_BIT : 0);
    return cueBurtRaw(theme, payloadB, hold ? " (HOLD)" : "");
}

FleetHandshake fleet_cue_burt_turn(ShowTheme theme, uint32_t turnBits) {
    return cueBurtRaw(theme, turnBits, " (BANTER turn)");
}

static FleetHandshake cueBurtRaw(ShowTheme theme, uint32_t payloadB, const char* label) {
    FleetHandshake r = { false, 0 };
    if (!s_espnowUp) return r;

    // fleet broadcast reaches Burt too (belt-and-suspenders) — send it first
    Msg b = makeMsg(CMD_NARRATOR_2_START);
    b.payload_a = theme; b.payload_b = payloadB; msg_seal(b);
    sendBroadcast3x(b);

    // additionally: unicast the SAME frame to Burt, check the native send result
    Msg u = makeMsg(CMD_NARRATOR_2_START, NODE_SKELETON_A1S);
    u.payload_a = theme; u.payload_b = payloadB; msg_seal(u);

    for (uint8_t i = 1; i <= 3; i++) {
        r.attempts = i;
        s_burtDone = false; s_burtOk = false;
        esp_err_t e = esp_now_send(MAC_SKELETON_A1S, (const uint8_t*)&u, sizeof(u));
        if (e == ESP_OK) {
            uint32_t t0 = millis();
            while (!s_burtDone && (millis() - t0) < 50) delay(1);   // ~50 ms budget
            if (s_burtDone && s_burtOk) { r.success = true; break; }
        }
        if (i < 3) delay(30);   // closely spaced, not backed off
    }

    s_hsCue_attempts += r.attempts;
    if (r.success) s_hsCue_ok++; else s_hsCue_fail++;
    ts(); Serial.printf("[fleet] H1b cue Burt%s: %s after %u attempt(s) — proceeding regardless\n",
                        label, r.success ? "SUCCESS" : "no confirmation", r.attempts);
    return r;
}

bool fleet_probe_burt() {
    // Presence check before starting a banter exchange: a unicast heartbeat
    // Burt ignores, judged ONLY by ESP-NOW's native delivery confirmation
    // (MAC-level ACK = Burt's radio is up and in range). Burt only announces
    // himself at boot, so there's no "heard recently" signal to use instead.
    if (!s_espnowUp) return false;
    Msg u = makeMsg(CMD_HEARTBEAT, NODE_SKELETON_A1S);
    for (uint8_t i = 1; i <= 3; i++) {
        s_burtDone = false; s_burtOk = false;
        if (esp_now_send(MAC_SKELETON_A1S, (const uint8_t*)&u, sizeof(u)) == ESP_OK) {
            uint32_t t0 = millis();
            while (!s_burtDone && (millis() - t0) < 50) delay(1);
            if (s_burtDone && s_burtOk) {
                ts(); Serial.printf("[fleet] Burt presence probe: OK (attempt %u)\n", i);
                return true;
            }
        }
        if (i < 3) delay(30);
    }
    ts(); Serial.println(F("[fleet] Burt presence probe: NO delivery confirmation (3 tries)"));
    return false;
}

void fleet_arm_burt_turn(uint32_t session, uint32_t turnBits) {
    s_turnMatched = false;
    s_turnPlayed  = false;
    s_turnSession = session;
    s_turnBits    = turnBits;
    s_turnArmed   = true;
}

int fleet_wait_burt_turn(uint32_t timeout_ms) {
    uint32_t t0 = millis();
    while (!s_turnMatched && (millis() - t0) < timeout_ms) delay(5);
    bool matched = s_turnMatched, played = s_turnPlayed;
    s_turnArmed = false;
    if (!matched) return -1;
    return played ? 1 : 0;
}

bool fleet_narrator2_done() {
    bool d = s_narrator2Done;
    s_narrator2Done = false;
    return d;
}

bool fleet_node_online_seen() {
    bool d = s_nodeOnlineSeen;
    s_nodeOnlineSeen = false;
    return d;
}

void fleet_print_handshake_log() {
    ts(); Serial.printf("[fleet] handshake rollup: cue ok=%lu fail=%lu (avg %.1f tries)  burt-acks-rx=%lu\n",
                        (unsigned long)s_hsCue_ok, (unsigned long)s_hsCue_fail,
                        (s_hsCue_ok + s_hsCue_fail) ? (float)s_hsCue_attempts / (s_hsCue_ok + s_hsCue_fail) : 0.0f,
                        (unsigned long)s_burtAckRx);
}

FleetHandshakeStats fleet_handshake_stats() {
    return { s_hsCue_ok, s_hsCue_fail, s_hsCue_attempts, s_burtAckRx };
}

// ── heartbeat ───────────────────────────────────────────────────────────────
void fleet_heartbeat_tick() {
    if (!s_espnowUp) return;
    if (millis() - s_lastHeartbeat < 10000) return;
    s_lastHeartbeat = millis();
    // 2026-09-24 (show-sync-orchestrator OI-2): the heartbeat used to be
    // CMD_SHOW_STATE(SHOW_IDLE), sent every 10s regardless of the real state,
    // so any prop that believed it thought the show ended 10s into the 210s
    // SHOW_ACTIVE window (patch-audio would drop its theme bed, the touchpad
    // would end the zombie DMX run, Singing Pumpkins would go idle). It's now
    // the fleet's existing CMD_HEARTBEAT (liveness only, no state; nothing
    // acts on it). NOT "the real current state" either: re-sending SHOW_ACTIVE
    // every 10s would restart a prop's ~3:00 theme track once it finished
    // inside the 3:30 window. Real state changes are still their own 3x
    // CMD_SHOW_STATE bursts (NARRATOR / ACTIVE / OFF+IDLE at reset).
    Msg m = makeMsg(CMD_HEARTBEAT); msg_seal(m);
    sendBroadcastOnce(m);              // heartbeat is a single frame, not a 3x burst
}

// ── coexistence self-test ───────────────────────────────────────────────────
bool fleet_coex_test(const char* url, bool with_playback) {
    ts(); Serial.printf("[coex] START  url=%s  playback=%d\n", url, with_playback);
    if (!s_wifiUp) { Serial.println(F("[coex] FAIL — WiFi not up")); return false; }

    int  chBefore = WiFi.channel();
    int  rssiBefore = WiFi.RSSI();
    uint32_t bcastSent = 0;
    int  httpCode = 0;
    int  bodyLen = 0, contentLen = -1;
    bool assocHeld = true;

    // interleave: ~60 broadcasts over ~3 s, HTTP GET kicked at iteration 12
    for (int i = 0; i < 60; i++) {
        Msg m = makeMsg(CMD_SHOW_STATE); m.payload_a = SHOW_PENDING; msg_seal(m);
        if (esp_now_send(MAC_BROADCAST, (const uint8_t*)&m, sizeof(m)) == ESP_OK) bcastSent++;

        if (with_playback) audio_out_tone(600, 8, 0.3f);

        if (i == 12) {
            HTTPClient http;
            http.setConnectTimeout(4000);
            http.setTimeout(6000);
            if (http.begin(url)) {
                httpCode = http.GET();
                contentLen = http.getSize();
                if (httpCode == 200) {
                    WiFiClient* st = http.getStreamPtr();
                    uint8_t buf[512];
                    uint32_t t0 = millis();
                    while (http.connected() && millis() - t0 < 8000) {
                        size_t avail = st->available();
                        if (avail) { int r = st->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail); bodyLen += r; }
                        else if (contentLen >= 0 && bodyLen >= contentLen) break;
                        else delay(2);
                    }
                }
                http.end();
            } else {
                Serial.println(F("[coex] http.begin failed"));
            }
        }

        if (WiFi.status() != WL_CONNECTED) assocHeld = false;
        if (!with_playback) delay(40);
    }

    int chAfter = WiFi.channel();
    bool chOk = (chAfter == WIFI_CHANNEL && chBefore == WIFI_CHANNEL);
    bool httpOk = (httpCode == 200) && (contentLen < 0 || bodyLen >= contentLen);

    ts(); Serial.printf("[coex] broadcasts sent=%lu   assoc held=%d   ch %d->%d (want %d)\n",
                        (unsigned long)bcastSent, assocHeld, chBefore, chAfter, WIFI_CHANNEL);
    ts(); Serial.printf("[coex] HTTP code=%d  content-len=%d  body-read=%d  RSSI %d->%d\n",
                        httpCode, contentLen, bodyLen, rssiBefore, WiFi.RSSI());
    bool pass = assocHeld && chOk && httpOk && bcastSent >= 55;
    ts(); Serial.printf("[coex] RESULT: %s%s\n", pass ? "PASS" : "FAIL",
                        pass ? " — WiFi STA + ESP-NOW TX coexist on ch 11 (leader role viable)" : "");
    return pass;
}

uint32_t fleet_tree_motion_ms()    { return s_treeMotionMs; }
uint32_t fleet_tree_motion_count() { return s_treeMotionRx; }
