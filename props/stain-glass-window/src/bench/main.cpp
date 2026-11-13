// Stain Glass + Front Door controller (wiring tests + projector power + player monitor).
// env:bench = this file alone, no show logic. env:show (-D SG_SHOW) adds the show listener (2026-10-07):
//   ESP-NOW from the Scarecrow -> shared ShowFollower.h -> at MAIN-SHOW (SHOW_ACTIVE) the latched theme fires:
//     door  : CORN / ROTLD / MUTANT over GPIO (gpio corn|rotld|mutant), ACK = status line steady HIGH
//     window: CORN / ROTLD -> "ACTION <THEME>" to the 4K1142, ACK = its "ACK ..." line within 3 s;
//             MUTANT -> window stays on its loop (logged)
//   Run ends when the door clip ends (status line back to toggling) or the follower's fail-safe deadline.
//   Console (show env): show = follower/radio status, sim <corn|rotld|mutant> = fake Scarecrow visit through
//   the real follower path (BENCH ONLY - never while visitors are in the yard).
//
// Automatic (2026-10-06, Joe):
//   * Projectors ON at boot and KEPT on: each projector is asked "power status?" over RS-232; OFF -> power ON,
//     COOLING -> wait (an ON during cooling is refused), no reply -> retry + ALARM. Re-checked every 60 s, so a
//     projector found OFF later (power blip, someone hit the remote) is turned back on. `poff` stops that for
//     the projector until `pon`. 5 ON attempts without reaching ON -> FAULT, then one try every 5 min.
//   * BrightSign players monitored: every line they send is printed ("[wb] < HB mode=LOOP ..."). Each player
//     sends READY at boot + HB every 30 s; silent 45 s -> we PING it; silent 75 s -> ALARM SILENT (repeated every
//     5 min), recovery -> "back". READY = player (re)booted, ERR lines = WARN. Our own command coming straight
//     back = ECHO (loopback in the harness / player not driving the line). Non-text bytes = GARBAGE (baud/wiring).
//   * Door player GPIO fallback (2026-10-06, the 3.5 mm serial link is dead): Joe's 7-pin plug on the XT1143
//     GPIO block pins 1-7. ESP32 GPIO18/19/23 -> player GPIO 0/1/2 (CORN/ROTLD/MUTANT): driven LOW 300 ms, else
//     high-Z (player has 1k pull-ups to 3.3 V). Player GPIO 3 -> ESP32 GPIO33 (pull-down): toggles every 1 s =
//     script alive in the loop, steady HIGH = action playing, steady LOW/no edges = DEAD. After a trigger, no
//     steady HIGH within 3 s = NO ACK. Tag "dg". Console: gpio <corn|rotld|mutant>.
//   * Every printed line starts with "[" (console artifact filter on the bridge keeps only "[" lines).
//
// USB console @115200, one command per line:
//   help                 this list
//   mon                  status table: projectors + players
//   flip <link>          swing that link's TX between idle and active every 2 s for 30 s.
//                        On the DE-9/3.5mm side the ESP32's TX pin swings ~-5 V <-> +5 V
//                        vs GND (pin 5). Proves which pin is ours before trusting labels.
//   status <wp|dp>       Hitachi "power status?" (changes nothing) + print the reply
//   pon <wp|dp>          Hitachi power ON + keep it on    poff <wp|dp>   power OFF + stop keeping it on
//   send <wb|db> <text>  send a text line (CR LF) to a BrightSign
//   swap <link>          swap that link's TX/RX pins in software (no rewiring; until reboot).
//                        Refused if the new TX would be input-only (GPIO34-39).
// Links: wb = window BrightSign, db = door BrightSign, wp = window projector, dp = door projector.
#include <Arduino.h>
#include <SoftwareSerial.h>
#ifdef SG_SHOW
#include <esp_system.h>
#include "ProjectConstants.h"
#include "ShowFollower.h"
#include "../show/espnow_rx.h"
#include "../show/ha_proj.h"
#endif

struct Link {
    const char* tag; const char* name;
    int tx, rx; uint32_t baud;
    Stream* s;
    bool hw; HardwareSerial* hs; EspSoftwareSerial::UART* ss;
};

static EspSoftwareSerial::UART swWinProj, swDoorProj;
static Link links[] = {
    {"wb", "window BrightSign 4K1142", 17, 16, 115200, nullptr, true,  &Serial2, nullptr},
    {"db", "door BrightSign XT1143",   22, 21, 115200, nullptr, true,  &Serial1, nullptr},
    {"wp", "window projector CP-X605", 26, 27, 19200,  nullptr, false, nullptr,  &swWinProj},
    {"dp", "door projector CP-DW10N",  32, 35, 19200,  nullptr, false, nullptr,  &swDoorProj},
};
static const int NLINKS = sizeof(links) / sizeof(links[0]);

// Hitachi RS-232 frames, verified on both real projectors 2026-10-04 (docs/stain-glass-window.md).
static const uint8_t HIT_STATUS[13] = {0xBE,0xEF,0x03,0x06,0x00,0x19,0xD3,0x02,0x00,0x00,0x60,0x00,0x00};
static const uint8_t HIT_ON[13]     = {0xBE,0xEF,0x03,0x06,0x00,0xBA,0xD2,0x01,0x00,0x00,0x60,0x01,0x00};
static const uint8_t HIT_OFF[13]    = {0xBE,0xEF,0x03,0x06,0x00,0x2A,0xD3,0x01,0x00,0x00,0x60,0x00,0x00};

// ---- projector keep-on --------------------------------------------------------------------------------
enum PState : uint8_t { P_UNKNOWN, P_OFF, P_ON, P_COOLING, P_NOREPLY };
static const char* pName(PState p) {
    return p == P_OFF ? "OFF" : p == P_ON ? "ON" : p == P_COOLING ? "COOLING" : p == P_NOREPLY ? "NO REPLY" : "?";
}
struct Proj {
    Link* l;
    bool wantOn = true;          // Joe 2026-10-06: both projectors on at boot
    PState st = P_UNKNOWN;
    uint32_t nextCheck = 0;      // millis()
    uint8_t onTries = 0;         // ON sent since last confirmed ON
    uint8_t noReply = 0;         // consecutive status queries without a reply
    bool fault = false;
    uint32_t lastOk = 0;         // last time it answered
};
static Proj projs[2];

static const uint32_t P_FIRST_MS   = 3000;     // first check after boot
static const uint32_t P_POLL_MS    = 60000;    // steady-state check
static const uint32_t P_FAST_MS    = 10000;    // after an ON / while unanswered
static const uint32_t P_COOL_MS    = 15000;    // re-check while cooling
static const uint32_t P_FAULT_MS   = 300000;   // after FAULT
static const uint8_t  P_MAX_TRIES  = 5;

// ---- player monitor -----------------------------------------------------------------------------------
struct Player {
    Link* l;
    String buf;                  // line being assembled
    uint32_t lastHeard = 0;      // millis() of last complete line (0 = never)
    String lastLine, mode;
    uint32_t hbs = 0, readys = 0, errs = 0, garbage = 0, echoes = 0;
    String lastSent; uint32_t lastSentAt = 0;
    bool silent = false;         // ALARM latched
    uint32_t lastPing = 0, lastAlarm = 0;
};
static Player players[2];
#ifdef SG_SHOW
static void showWindowLine(Player& p, const String& line);
#endif
static const uint32_t PL_PING_MS  = 45000;     // HB is every 30 s
static const uint32_t PL_ALARM_MS = 75000;
static const uint32_t PL_REALARM_MS = 300000;
static uint32_t bootMs;
static void tstamp() { Serial.printf("[%7lus] ", (unsigned long)(millis() / 1000)); }

// ---- door player over GPIO ----------------------------------------------------------------------------
static const int DG_OUT[3] = {18, 19, 23};            // -> player GPIO 0 / 1 / 2
static const char* DG_THEME[3] = {"corn", "rotld", "mutant"};
static const int DG_STATUS = 33;                      // <- player GPIO 3
static const uint32_t DG_PULSE_MS = 300, DG_EDGE_MS = 2500, DG_ACK_MS = 3000, DG_STUCK_MS = 200000;
enum DgState : uint8_t { DG_UNKNOWN, DG_ALIVE, DG_PLAYING, DG_DEAD, DG_STUCK };
static const char* dgName(DgState d) {
    return d == DG_ALIVE ? "ALIVE (loop)" : d == DG_PLAYING ? "ACTION playing" : d == DG_DEAD ? "DEAD (no toggling)"
         : d == DG_STUCK ? "STUCK HIGH" : "?";
}
static struct {
    int level = -1; uint32_t lastEdge = 0, highSince = 0, edges = 0;
    DgState st = DG_UNKNOWN;
    int pulsePin = -1; uint32_t pulseEnd = 0;
    int ackFor = -1; uint32_t ackBy = 0;
    uint32_t triggers = 0, acks = 0, noAcks = 0;
} dg;

#ifdef SG_SHOW
static void showDoorClipEnded();
#endif

static void dgTrigger(int i) {
    if (dg.pulsePin >= 0) { Serial.println(F("[dg] pulse already in progress")); return; }
    pinMode(DG_OUT[i], OUTPUT); digitalWrite(DG_OUT[i], LOW);
    dg.pulsePin = DG_OUT[i]; dg.pulseEnd = millis() + DG_PULSE_MS;
    dg.ackFor = i; dg.ackBy = millis() + DG_ACK_MS; dg.triggers++;
    tstamp(); Serial.printf("[dg] > %s (GPIO%d low %lu ms -> player GPIO %d)\n", DG_THEME[i], DG_OUT[i],
                            (unsigned long)DG_PULSE_MS, i);
}

static void dgTick() {
    uint32_t now = millis();
    if (dg.pulsePin >= 0 && (int32_t)(now - dg.pulseEnd) >= 0) { pinMode(dg.pulsePin, INPUT); dg.pulsePin = -1; }
    int lv = digitalRead(DG_STATUS);
    if (lv != dg.level) {
        if (dg.level >= 0) { dg.lastEdge = now; dg.edges++; }
        if (lv) dg.highSince = now;
        dg.level = lv;
    }
    bool toggling = dg.edges && now - dg.lastEdge < DG_EDGE_MS;
    DgState st = toggling ? DG_ALIVE
               : lv ? (now - dg.highSince > DG_STUCK_MS ? DG_STUCK : DG_PLAYING) : DG_DEAD;
    if (now - bootMs < 4000) return;                  // let the line settle after boot
    if (dg.ackFor >= 0) {
        if (lv && !toggling && now - dg.highSince > 1200) {
            dg.acks++; tstamp(); Serial.printf("[dg] ACK %s: status steady HIGH = clip playing\n", DG_THEME[dg.ackFor]);
            dg.ackFor = -1;
        } else if ((int32_t)(now - dg.ackBy) >= 0) {
            dg.noAcks++; tstamp(); Serial.printf("[dg] NO ACK for %s within 3 s (status %s)\n", DG_THEME[dg.ackFor], dgName(st));
            dg.ackFor = -1;
        }
    }
    if (st != dg.st) {
#ifdef SG_SHOW
        if (dg.st == DG_PLAYING && st == DG_ALIVE) showDoorClipEnded();
#endif
        tstamp();
        Serial.printf("[dg] door player status line: %s (was %s)%s\n", dgName(st), dgName(dg.st),
                      st == DG_DEAD ? " - ALARM: player off / script stopped / wire?" :
                      st == DG_STUCK ? " - ALARM: high > 200 s, longer than any clip" : "");
        dg.st = st;
    }
}

static void linkBegin(Link& l) {
    if (l.hw) { l.hs->setRxBufferSize(1024); l.hs->begin(l.baud, SERIAL_8N1, l.rx, l.tx); l.s = l.hs; }
    else      { l.ss->begin(l.baud, EspSoftwareSerial::SWSERIAL_8N1, l.rx, l.tx); l.s = l.ss; }
}

static void linkEnd(Link& l) {
    if (l.hw) l.hs->end(); else l.ss->end();
    l.s = nullptr;
}

static Link* find(const String& tag) {
    for (auto& l : links) if (tag == l.tag) return &l;
    return nullptr;
}
static Proj* projFor(Link* l)     { for (auto& p : projs) if (p.l == l) return &p; return nullptr; }
static Player* playerFor(Link* l) { for (auto& p : players) if (p.l == l) return &p; return nullptr; }


static void help() {
    Serial.println(F("[help] door GPIO: gpio <corn|rotld|mutant>  (GPIO18/19/23 -> player 0/1/2, status <- GPIO33)"));
    Serial.println(F("[help] commands: mon, flip <wb|db|wp|dp>, status <wp|dp>, pon <wp|dp>, poff <wp|dp>,"));
    Serial.println(F("[help]           send <wb|db> <text>, swap <link>, help"));
    for (auto& l : links)
        Serial.printf("[help]   %s = %-26s TX GPIO%-2d RX GPIO%-2d %lu baud %s\n", l.tag, l.name, l.tx, l.rx,
                      (unsigned long)l.baud, l.hw ? "(hardware UART)" : "(software serial)");
}

static void mon() {
    uint32_t now = millis();
    Serial.printf("[mon] uptime %lus\n", (unsigned long)((now - bootMs) / 1000));
    for (auto& p : projs)
        Serial.printf("[mon] %s %-26s power %-8s keep-on %-3s%s tries %u, last answer %s\n", p.l->tag, p.l->name,
                      pName(p.st), p.wantOn ? "yes" : "no", p.fault ? " FAULT" : "", p.onTries,
                      p.lastOk ? (String((now - p.lastOk) / 1000) + "s ago").c_str() : "never");
    for (auto& p : players)
        Serial.printf("[mon] %s %-26s %s, last heard %s, mode %s | HB %lu READY %lu ERR %lu echo %lu garbage %lu | \"%s\"\n",
                      p.l->tag, p.l->name, p.silent ? "SILENT" : (p.lastHeard ? "OK" : "waiting"),
                      p.lastHeard ? (String((now - p.lastHeard) / 1000) + "s ago").c_str() : "never",
                      p.mode.length() ? p.mode.c_str() : "?", (unsigned long)p.hbs, (unsigned long)p.readys,
                      (unsigned long)p.errs, (unsigned long)p.echoes, (unsigned long)p.garbage, p.lastLine.c_str());
    Serial.printf("[mon] dg door player GPIO           status %s, edges %lu | triggers %lu ACK %lu NO-ACK %lu\n",
                  dgName(dg.st), (unsigned long)dg.edges, (unsigned long)dg.triggers, (unsigned long)dg.acks,
                  (unsigned long)dg.noAcks);
}

static void flip(Link& l) {
    Serial.printf("[%s] flip: TX GPIO%d swings every 2 s for 30 s. Meter DE-9 pins 2 and 3 vs 5:\n", l.tag, l.tx);
    Serial.printf("[%s]   the pin that follows IDLE ~-5 V / ACTIVE ~+5 V is the ESP32's TX.\n", l.tag);
    linkEnd(l);
    pinMode(l.tx, OUTPUT);
    for (int i = 0; i < 15; i++) {
        bool idle = (i % 2 == 0);
        digitalWrite(l.tx, idle ? HIGH : LOW);       // UART idle = HIGH -> RS-232 about -5 V
        Serial.printf("[%s]   %s (expect %s)\n", l.tag, idle ? "IDLE  " : "ACTIVE", idle ? "-5 V" : "+5 V");
        delay(2000);
    }
    digitalWrite(l.tx, HIGH);
    linkBegin(l);
    Serial.printf("[%s] flip done, link restarted\n", l.tag);
}

// Send one Hitachi frame, wait up to 1 s for the reply. Returns reply length (0 = none).
static int hitachiRaw(Link& l, const uint8_t* frame, uint8_t* buf, int cap) {
    while (l.s->available()) l.s->read();             // drop stale bytes
    l.s->write(frame, 13);
    int n = 0; uint32_t t0 = millis();
    while (millis() - t0 < 1000 && n < cap) {
        if (l.s->available()) { buf[n++] = l.s->read(); if (buf[0] != 0x1D || n >= 3) break; }
        else delay(1);
    }
    return n;
}

static void decode(const uint8_t* buf, int n) {
    for (int i = 0; i < n; i++) Serial.printf(" %02X", buf[i]);
    if (buf[0] == 0x06) Serial.print(F("  = ACK (command accepted)"));
    else if (buf[0] == 0x15) Serial.print(F("  = NAK (not understood)"));
    else if (buf[0] == 0x1D && n >= 2)
        Serial.printf("  = power %s", buf[1] == 0 ? "OFF" : buf[1] == 1 ? "ON" : buf[1] == 2 ? "COOLING" : "?");
    else if (buf[0] == 0x1C) Serial.print(F("  = BUSY"));
}

static void hitachi(Link& l, const uint8_t* frame, const char* what) {   // console version, verbose
    uint8_t buf[16];
    int n = hitachiRaw(l, frame, buf, sizeof buf);
    if (n == 0) { Serial.printf("[%s] %s: NO REPLY (check TX/RX swap, cable cross, projector baud 19200)\n", l.tag, what); return; }
    Serial.printf("[%s] %s reply:", l.tag, what);
    decode(buf, n);
    Serial.println();
}

// Query power state. Returns P_NOREPLY if nothing (or nothing sensible) came back.
static PState queryPower(Proj& p) {
    uint8_t buf[16];
    int n = hitachiRaw(*p.l, HIT_STATUS, buf, sizeof buf);
    if (n >= 2 && buf[0] == 0x1D) {
        p.lastOk = millis();
        return buf[1] == 0 ? P_OFF : buf[1] == 1 ? P_ON : buf[1] == 2 ? P_COOLING : P_UNKNOWN;
    }
    if (n) { tstamp(); Serial.printf("[%s] unexpected status reply:", p.l->tag); decode(buf, n); Serial.println(); }
    return P_NOREPLY;
}

static void projTick(Proj& p) {
    uint32_t now = millis();
    if ((int32_t)(now - p.nextCheck) < 0) return;
    PState was = p.st;
    PState st = queryPower(p);
    p.st = st;

    if (st == P_NOREPLY) {
        p.noReply++;
        if (p.noReply == 1 || p.noReply == 3 || p.noReply % 30 == 0) {
            tstamp();
            Serial.printf("[%s] %s: NO REPLY to power status (%u in a row)%s\n", p.l->tag, p.l->name, p.noReply,
                          p.noReply >= 3 ? " - ALARM: projector unplugged / RS-232 cable off?" : ", retrying");
        }
        p.nextCheck = now + (p.noReply < 6 ? P_FAST_MS : P_POLL_MS);
        return;
    }
    if (p.noReply >= 3) { tstamp(); Serial.printf("[%s] answering again after %u missed checks\n", p.l->tag, p.noReply); }
    p.noReply = 0;
    if (st != was) { tstamp(); Serial.printf("[%s] power %s (was %s)\n", p.l->tag, pName(st), pName(was)); }

    if (st == P_ON) {
        if (p.onTries || p.fault) { tstamp(); Serial.printf("[%s] confirmed ON\n", p.l->tag); }
        p.onTries = 0; p.fault = false;
        p.nextCheck = now + P_POLL_MS;
        return;
    }
    if (!p.wantOn) { p.nextCheck = now + (st == P_COOLING ? P_COOL_MS : P_POLL_MS); return; }   // poff: just watch
    if (st == P_COOLING) { p.nextCheck = now + P_COOL_MS; return; }   // ON is refused while cooling

    // OFF (or unknown) and it should be on
    if (p.onTries >= P_MAX_TRIES) {
        if (!p.fault) {
            p.fault = true;
            tstamp();
            Serial.printf("[%s] FAULT: %u power-ON attempts, still %s - trying again every 5 min\n", p.l->tag,
                          p.onTries, pName(st));
        }
        p.onTries = P_MAX_TRIES - 1;     // allow one more try next time
        p.nextCheck = now + P_FAULT_MS;
        return;
    }
    uint8_t buf[16];
    int n = hitachiRaw(*p.l, HIT_ON, buf, sizeof buf);
    p.onTries++;
    tstamp();
    Serial.printf("[%s] AUTO power ON (attempt %u, was %s%s):", p.l->tag, p.onTries, pName(st),
                  was == P_ON ? " - it was ON before, turned off?" : "");
    if (n) decode(buf, n); else Serial.print(F(" no reply"));
    Serial.println();
    p.nextCheck = now + P_FAST_MS;
}

// pon / poff, shared by the console and Home Assistant (2026-10-20). OFF = Hitachi standby: the lamp cools
// itself (~1-2 min, status COOLING -> OFF); keep-on stops so it isn't powered back on.
static void projSet(Proj& p, bool on, const char* who) {
    p.wantOn = on;
    if (on) { p.onTries = 0; p.fault = false; }
    p.nextCheck = millis() + P_FAST_MS;
    tstamp(); Serial.printf("[%s] %s requested by %s\n", p.l->tag, on ? "ON" : "OFF (standby, lamp cools)", who);
    hitachi(*p.l, on ? HIT_ON : HIT_OFF, on ? "POWER ON (keep-on enabled)" : "POWER OFF (keep-on disabled until pon)");
}

#ifdef SG_SHOW
bool sgProjWant(int i) { return projs[i].wantOn; }
const char* sgProjState(int i) { return projs[i].fault ? "FAULT" : pName(projs[i].st); }
void sgProjSet(int i, bool on, const char* who) { projSet(projs[i], on, who); }
#endif

// ---- players ------------------------------------------------------------------------------------------
static void playerSend(Player& p, const String& text) {
    p.l->s->print(text); p.l->s->print("\r\n");
    p.lastSent = text; p.lastSentAt = millis();
}

static void playerLine(Player& p, String line) {
    uint32_t now = millis();
    line.trim();
    if (!line.length()) return;
    if (p.lastSent.length() && line == p.lastSent && now - p.lastSentAt < 1000) {
        p.echoes++;
        tstamp();
        Serial.printf("[%s] ECHO of our own \"%s\" - not the player (harness loopback / player not on the line?)\n",
                      p.l->tag, line.c_str());
        return;
    }
    if (p.silent) {
        tstamp();
        Serial.printf("[%s] player back after %lus silent\n", p.l->tag,
                      (unsigned long)((now - p.lastHeard) / 1000));
        p.silent = false;
    }
    if (!p.lastHeard) { tstamp(); Serial.printf("[%s] first line from the player - link OK\n", p.l->tag); }
    p.lastHeard = now;
    p.lastLine = line;
    int m = line.indexOf("mode=");
    if (m >= 0) { int e = line.indexOf(' ', m); p.mode = line.substring(m + 5, e < 0 ? line.length() : e); }

#ifdef SG_SHOW
    showWindowLine(p, line);
#endif
    const char* note = "";
    if (line.startsWith("HB")) p.hbs++;
    else if (line.startsWith("READY")) { p.readys++; note = "   <- player (re)booted"; }
    else if (line.startsWith("ERR")) { p.errs++; note = "   <- WARN"; }
    tstamp();
    Serial.printf("[%s] < %s%s\n", p.l->tag, line.c_str(), note);
}

static void playerRx(Player& p) {
    uint32_t junk = 0;
    while (p.l->s && p.l->s->available()) {
        int c = p.l->s->read();
        if (c == '\r' || c == '\n') { if (p.buf.length()) playerLine(p, p.buf); p.buf = ""; }
        else if (c >= 32 && c < 127) { if (p.buf.length() < 200) p.buf += (char)c; }
        else junk++;
    }
    if (junk) {
        p.garbage += junk;
        tstamp();
        Serial.printf("[%s] GARBAGE: %lu non-text byte(s) (wrong baud / wiring / noise) - total %lu\n", p.l->tag,
                      (unsigned long)junk, (unsigned long)p.garbage);
    }
}

static void playerTick(Player& p) {
    uint32_t now = millis();
    uint32_t since = p.lastHeard ? p.lastHeard : bootMs;
    uint32_t quiet = now - since;
    if (quiet >= PL_PING_MS && now - p.lastPing >= PL_PING_MS) {
        p.lastPing = now;
        playerSend(p, "PING");
    }
    if (quiet >= PL_ALARM_MS && (!p.silent || now - p.lastAlarm >= PL_REALARM_MS)) {
        p.silent = true; p.lastAlarm = now;
        tstamp();
        Serial.printf("[%s] ALARM SILENT: %s %s for %lus (expect HB every 30 s) - card in? player powered? serial?\n",
                      p.l->tag, p.l->name, p.lastHeard ? "quiet" : "never heard since boot",
                      (unsigned long)(quiet / 1000));
    }
}


#ifdef SG_SHOW
// ---- show listener (env:show) -------------------------------------------------------------------------
static_assert(showfollow::W_CMD_START_SCENE == CMD_START_SCENE && showfollow::W_CMD_SET_THEME == CMD_SET_THEME &&
              showfollow::W_CMD_SHOW_STATE == CMD_SHOW_STATE && showfollow::W_CMD_HEARTBEAT == CMD_HEARTBEAT &&
              showfollow::W_NODE_SCARECROW == NODE_SCARECROW_A1S && showfollow::W_SHOW_NARRATOR == SHOW_NARRATOR &&
              showfollow::W_SHOW_ACTIVE == SHOW_ACTIVE && showfollow::W_SHOW_OFF == SHOW_OFF &&
              showfollow::W_SHOW_IDLE == SHOW_IDLE && showfollow::W_THEME_COUNT == THEME_MUTANT_PUMPKINS + 1,
              "ShowFollower.h wire values drifted from ProjectConstants.h");
static_assert(THEME_CORN == 0 && THEME_ROTLD == 1 && THEME_MUTANT_PUMPKINS == 2, "DG_THEME[] index = theme value");

static const char* THEME_UP[3] = {"CORN", "ROTLD", "MUTANT"};
static showfollow::Config followCfg() {
    showfollow::Config c;
    c.showActiveMs = SHOW_ACTIVE_MS;
    // The run follows the DOOR clip (status line HIGH -> toggling). Door clips are ~3:00 (CORN 16:22:55 -> 16:25:55
    // on 2026-10-07). Window clips are shorter and not tracked here (it reports DONE / NOW LOOP itself).
    for (auto& t : c.expectedTrackMs) t = 180000;
    return c;
}
static showfollow::Follower g_follow(followCfg());
static struct {
    uint32_t wbAckBy = 0; const char* wbAckFor = nullptr;
    uint32_t fires = 0, wbAcks = 0, wbNoAcks = 0, wbRefused = 0, doorDeadAtFire = 0;
    uint32_t lastFrameLog = 0;
} sh;

static bool verifyNodeMac() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (memcmp(mac, MAC_STAIN_GLASS, 6) == 0) {
        Serial.printf("[boot] MAC verified %02X:%02X:%02X:%02X:%02X:%02X = Stain Glass\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return true;
    }
    for (;;) {   // wrong board: never act on show cues meant for another prop
        Serial.printf("[FATAL] MAC %02X:%02X:%02X:%02X:%02X:%02X is not MAC_STAIN_GLASS - wrong firmware on this board\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        delay(5000);
    }
}

// Fire one theme on both projections.
static void showFire(uint8_t theme, const char* why) {
    sh.fires++;
    tstamp(); Serial.printf("[show] FIRE %s (%s)\n", THEME_UP[theme], why);
    if (dg.st == DG_DEAD || dg.st == DG_STUCK) {
        sh.doorDeadAtFire++;
        tstamp(); Serial.printf("[show] WARN door player status is %s - firing anyway, expect NO ACK\n", dgName(dg.st));
    }
    dgTrigger(theme);
    if (theme == THEME_MUTANT_PUMPKINS) {
        tstamp(); Serial.println(F("[show] window: no MUTANT action - window stays on its loop"));
        return;
    }
    playerSend(players[0], String("ACTION ") + THEME_UP[theme]);
    sh.wbAckBy = millis() + 3000; if (!sh.wbAckBy) sh.wbAckBy = 1;
    sh.wbAckFor = THEME_UP[theme];
    tstamp(); Serial.printf("[wb] > ACTION %s\n", THEME_UP[theme]);
}

static void applyFollower(const showfollow::Out& o) {
    if (o.why) {
        tstamp();
        Serial.printf("[show] %s%s (phase %s, theme %s)\n", o.warn ? "WARN " : "", o.why,
                      showfollow::Follower::phaseName(g_follow.phase()), THEME_UP[g_follow.theme() % 3]);
    }
    if (o.act == showfollow::Act::PLAY) showFire(o.theme, "MAIN-SHOW");
    else if (o.act == showfollow::Act::TO_IDLE) { tstamp(); Serial.println(F("[show] follower run ended - players go back to their loops on their own when their clips end")); }
}

static void showDoorClipEnded() {
    if (g_follow.isPlaying()) applyFollower(g_follow.onTrackEnd(millis()));
}

static void showWindowLine(Player& p, const String& line) {
    if (&p != &players[0] || !sh.wbAckBy) return;
    if (line.startsWith("ACK")) {
        sh.wbAcks++; sh.wbAckBy = 0;
        tstamp(); Serial.printf("[show] window ACK %s\n", sh.wbAckFor);
    } else if (line.startsWith("BUSY") || line.startsWith("ERR")) {
        sh.wbRefused++; sh.wbAckBy = 0;
        tstamp(); Serial.printf("[show] WARN window refused ACTION %s: %s\n", sh.wbAckFor, line.c_str());
    }
}

static void onShowFrame(const Msg& m) {
    uint32_t now = millis();
    if (m.command != CMD_HEARTBEAT) {
        tstamp();
        Serial.printf("[rx] cmd 0x%02X from node 0x%02X session %08lX a=%lu\n", m.command, m.sender,
                      (unsigned long)m.session_id, (unsigned long)m.payload_a);
    }
    applyFollower(g_follow.onFrame(m.command, m.sender, m.session_id, m.payload_a, now));
}

static void showTick() {
    espnowRxTick();
    applyFollower(g_follow.tick(millis()));
    if (sh.wbAckBy && (int32_t)(millis() - sh.wbAckBy) >= 0) {
        sh.wbNoAcks++; sh.wbAckBy = 0;
        tstamp(); Serial.printf("[show] WARN window NO ACK for ACTION %s within 3 s (player %s)\n", sh.wbAckFor,
                                players[0].silent ? "SILENT" : "heard recently");
    }
}

static void showMon() {
    uint32_t now = millis();
    EspNowRxStats r = espnowRxStats();
    const showfollow::Stats& st = g_follow.stats();
    Serial.printf("[mon] show phase %s, theme %s, Scarecrow %s | fires %lu, door-dead-at-fire %lu, window ACK %lu NO-ACK %lu refused %lu\n",
                  showfollow::Follower::phaseName(g_follow.phase()), THEME_UP[g_follow.theme() % 3],
                  r.lastScarecrowMs ? (String((now - r.lastScarecrowMs) / 1000) + "s ago").c_str() : "never heard",
                  (unsigned long)sh.fires, (unsigned long)sh.doorDeadAtFire, (unsigned long)sh.wbAcks,
                  (unsigned long)sh.wbNoAcks, (unsigned long)sh.wbRefused);
    Serial.printf("[mon] radio %s ch %u | rx %lu queued %lu badLen %lu badSeal %lu notUs %lu dropped %lu relocks %lu | follower plays %lu replaces %lu dup %lu badSender %lu deadline %lu\n",
                  r.ready ? "UP" : "DOWN", r.channel, (unsigned long)r.rxAll, (unsigned long)r.rxQueued,
                  (unsigned long)r.rxBadLen, (unsigned long)r.rxBadSeal, (unsigned long)r.rxNotUs,
                  (unsigned long)r.qDropped, (unsigned long)r.chRelocks, (unsigned long)st.plays,
                  (unsigned long)st.replaces, (unsigned long)st.dupIgnored, (unsigned long)st.badSender,
                  (unsigned long)st.deadlineHits);
}

// Bench: a fake Scarecrow visit through the real follower path (theme, narrator, then MAIN-SHOW).
static void showSim(uint8_t theme) {
    uint32_t sess = esp_random();
    tstamp(); Serial.printf("[show] SIM visit %s (bench only) session %08lX\n", THEME_UP[theme], (unsigned long)sess);
    Msg m = {};
    m.sender = NODE_SCARECROW_A1S; m.target = NODE_BROADCAST; m.session_id = sess;
    m.command = CMD_SET_THEME;  m.payload_a = theme;          onShowFrame(m);
    m.command = CMD_SHOW_STATE; m.payload_a = SHOW_NARRATOR;  onShowFrame(m);
    m.command = CMD_SHOW_STATE; m.payload_a = SHOW_ACTIVE;    onShowFrame(m);
}
#endif

// ---- console ------------------------------------------------------------------------------------------
static void handle(String line) {
    line.trim();
    if (!line.length()) return;
    int sp = line.indexOf(' ');
    String cmd = sp < 0 ? line : line.substring(0, sp);
    String rest = sp < 0 ? "" : line.substring(sp + 1);
    rest.trim();
    String arg = rest.indexOf(' ') < 0 ? rest : rest.substring(0, rest.indexOf(' '));
    cmd.toLowerCase(); arg.toLowerCase();

    if (cmd == "help") { help(); return; }
    if (cmd == "mon") {
        mon();
#ifdef SG_SHOW
        showMon();
        haProjMon();
#endif
        return;
    }
#ifdef SG_SHOW
    if (cmd == "show") { showMon(); return; }
    if (cmd == "sim") {
        for (int i = 0; i < 3; i++) if (arg == DG_THEME[i]) { showSim(i); return; }
        Serial.println(F("[err] sim <corn|rotld|mutant>")); return;
    }
#endif
    if (cmd == "gpio") {
        for (int i = 0; i < 3; i++) if (arg == DG_THEME[i]) { dgTrigger(i); return; }
        Serial.println(F("[err] gpio <corn|rotld|mutant>")); return;
    }
    Link* l = find(arg);
    if (!l) { Serial.printf("[err] unknown link '%s'\n", arg.c_str()); help(); return; }
    if (cmd == "flip") { flip(*l); return; }
    if (cmd == "swap") {
        if (l->rx >= 34) { Serial.printf("[%s] can't swap: GPIO%d is input-only, can't be TX\n", l->tag, l->rx); return; }
        linkEnd(*l); int t = l->tx; l->tx = l->rx; l->rx = t; linkBegin(*l);
        Serial.printf("[%s] SWAPPED: now TX GPIO%d, RX GPIO%d (until reboot)\n", l->tag, l->tx, l->rx); return;
    }
    Proj* pj = projFor(l);
    Player* pl = playerFor(l);
    if (cmd == "status" && pj) { hitachi(*l, HIT_STATUS, "POWER STATUS?"); return; }
    if (cmd == "pon" && pj) { projSet(*pj, true, "console"); return; }
    if (cmd == "poff" && pj) { projSet(*pj, false, "console"); return; }
    if (cmd == "send" && pl) {
        String text = rest.substring(arg.length()); text.trim();
        playerSend(*pl, text);
        Serial.printf("[%s] > %s\n", l->tag, text.c_str()); return;
    }
    Serial.println(F("[err] command doesn't fit that link (status/pon/poff = wp|dp, send = wb|db)"));
}

void setup() {
    Serial.begin(115200);
    delay(300);
    bootMs = millis();
#ifdef SG_SHOW
    Serial.println(F("\n[boot] === Stain Glass + Front Door controller - SHOW (follows the Scarecrow) ==="));
#ifdef FW_GIT
    Serial.println(F("[boot] fw " FW_GIT));
#endif
    verifyNodeMac();
#else
    Serial.println(F("\n[boot] === Stain Glass + Front Door controller (bench + keep-on + player monitor) ==="));
#endif
    for (auto& l : links) linkBegin(l);
    for (int i = 0; i < 3; i++) pinMode(DG_OUT[i], INPUT);   // high-Z; player's 1k pull-ups hold them high
    pinMode(DG_STATUS, INPUT_PULLDOWN);                      // unplugged = LOW = DEAD, never a false ALIVE
    projs[0].l = find("wp"); projs[1].l = find("dp");
    players[0].l = find("wb"); players[1].l = find("db");
    projs[0].nextCheck = P_FIRST_MS;
    projs[1].nextCheck = P_FIRST_MS + 1500;
    help();
    Serial.println(F("[boot] projectors: power ON + keep on; players: STATUS asked, HB watched"));
    for (auto& p : players) playerSend(p, "STATUS");   // a player already running answers now
#ifdef SG_SHOW
    espnowRxInit(onShowFrame);
    haProjBegin();                         // after espnowRxInit: WiFi is already STA on the mesh channel
    Serial.println(F("[help] show: show (status), sim <corn|rotld|mutant> (BENCH ONLY fake visit)"));
#endif
}

void loop() {
    static String line;
    static uint32_t lastMon = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') { if (line.length()) handle(line); line = ""; }
        else line += c;
    }
    for (auto& p : players) { playerRx(p); playerTick(p); }
    for (auto& p : projs) projTick(p);
    dgTick();
#ifdef SG_SHOW
    showTick();
    haProjTick();
#endif
    for (auto& p : projs)                  // projector bytes outside a query (should be none)
        while (p.l->s && p.l->s->available()) {
            tstamp(); Serial.printf("[%s] stray byte %02X\n", p.l->tag, p.l->s->read());
        }
    if (millis() - lastMon >= 300000) {    // summary every 5 min
        lastMon = millis(); mon();
#ifdef SG_SHOW
        showMon();
        haProjMon();
#endif
    }
}
