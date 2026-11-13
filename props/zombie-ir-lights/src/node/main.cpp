// =============================================================================
// Zombie Container IR lights — ESP-01S IR transmitter node
// =============================================================================
// Drives the Zombie Container's 44-key-remote LED controller over IR so its
// lights follow the PicoDMX show. Listen-only on ESP-NOW; never transmits on
// the mesh.
//
//   all night (idle)        SWAMP   green breathing + a touch of aqua
//   show 0:00  -> 1:13.40   SWAMP   CONTAINED
//   show 1:13.40 -> 1:20.10 RED     WARNING: red alert fade
//   show 1:20.10 -> 2:49.86 ZOMBIE  glass break -> random white strobe, dark between
//   show 2:49.86 ->         SWAMP   reset, back to the night loop
// Boundaries = docs/zombie-container-dmx-timeline.md (Joe's measured show),
// copied from props/touchpad-ota-controller/src/dmx_timeline.h.
//
// TRIGGER: identical to props/zombie-relay-esp01s (the board that actually
// fires the PicoDMX), so the lights start on the same frame the show does:
//   1. Scarecrow's CMD_SET_THEME = THEME_ROTLD      (the relay's primary)
//   2. touchpad's CMD_SEGMENT_START = SEGMENT_ZOMBIE (the relay's backdoor)
//   with the same per-session_id dedupe.
//
// FAILSAFES (deadman). Every path lands back on SWAMP, the safe default:
//   - The show clock is local. At 2:49.86 the node returns to SWAMP whether or
//     not anything else is heard. (The touchpad sends no END when the show
//     finishes normally, so nothing is waited on.)
//   - CMD_SEGMENT_END (touchpad e-stop / reset) or CMD_SHOW_STATE IDLE|OFF
//     (Scarecrow reset): back to SWAMP now. Mirrors the touchpad's own LED.
//   - Hard 5-min cap on any show state (SHOW_MAX_MS), separate from the
//     clock, so the node can't get stuck in RED or ZOMBIE whatever arrives.
//   - While a show runs, new starts are ignored (same lockout as the
//     touchpad), so repeated starts can't keep it in a show forever.
//   - A missed start means the node just stays on SWAMP.
//   - Boot / any reset = SWAMP. Daily preventive restart while idle
//     (no OTA, not meant to be pulled). The ESP8266 hardware/software
//     watchdogs reset a hung loop, which also lands on SWAMP.
//   - Every command is idempotent: colors, DIY1 black, brightness clamped at
//     min/max. The fades re-assert the dim floor every breath and re-send the
//     color. A lost or doubled IR frame, or the controller losing power, heals
//     within one cycle. POWER (a toggle) is NEVER sent.
//
// HARDWARE (bench-proven 2026-09-30 on an ESP32 with the same module):
//   IR TX module S -> GPIO3 (RX pin; not a boot strap). Module VCC 5V, GND
//   shared with the ESP-01S. Before wiring, confirm S reads ~0V with the
//   module on 5V and S unconnected (5V on S would hurt the ESP).
//   The LED has a ~20 degree beam and ~1.3 m range at 5V. Mount it fixed,
//   pointed straight at the controller's IR eye; off-angle looks exactly like
//   a dead module.
//   Controller DIY1 must stay mixed to black (0,0,0). Joe set it 09-30.
//   GPIO3 is the UART RX pin, so serial is TX-only: logs only, no commands.
//
// Codes: props/ir-learner/codes.md (NEC, captured + replay-verified).
// =============================================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
extern "C" {
#include <espnow.h>
#include <user_interface.h>
}
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_NEC.h>
#include "ProjectConstants.h"
#include "wifi_credentials.h"
#include "segment_protocol.h"
#include "bench_frame.h"

// ── show timeline, ms from the trigger (dmx_timeline.h) ─────────────────────
static const uint32_t T_RED_START_MS    =  73400;   // 1:13.40 WARNING
static const uint32_t T_GLASS_BREAK_MS  =  80100;   // 1:20.10 strobe #1 / air cannon
static const uint32_t T_RESET_MS        = 169860;   // 2:49.86 green returns

// Hard backstop, independent of the timeline logic above (Joe 09-30: "it
// should not get stuck in it for the rest of time"). No show state outlives
// this, whatever arrives or doesn't arrive over ESP-NOW.
static const uint32_t SHOW_MAX_MS       = 300000;   // 5 min
// No OTA, and the board is not meant to be pulled (Joe). A preventive restart
// once a day, only while idle on SWAMP, clears any slow leak or wedged radio
// state. Boot goes straight back to SWAMP, so it is invisible.
static const uint32_t IDLE_REBOOT_MS    = 24UL * 3600UL * 1000UL;

// ── IR codes (44-key remote, NEC 32-bit) ────────────────────────────────────
static const uint32_t IR_RED        = 0xFF1AE5;   // verified 09-30
static const uint32_t IR_GREEN      = 0xFF9A65;   // verified
static const uint32_t IR_AQUA       = 0xFF8A75;   // verified (swamp accent)
static const uint32_t IR_WHITE      = 0xFF22DD;   // verified
static const uint32_t IR_FLASH      = 0xFFD02F;   // verified
static const uint32_t IR_QUICK      = 0xFFE817;   // verified (W preset "perfect")
static const uint32_t IR_BRIGHT_UP  = 0xFF3AC5;   // captured + verified
static const uint32_t IR_BRIGHT_DN  = 0xFFBA45;   // verified in the fades
static const uint32_t IR_DARK_DIY1  = 0xFF30CF;   // DIY1 = black, verified

static const uint8_t  IR_PIN        = 3;          // GPIO3 (RX)
static const uint16_t FAST_GAP_US   = 20000;      // short NEC gap for brightness steps (bench-proven)
static const uint8_t  FLOOR_EXTRA   = 1;          // extra DOWNs to re-assert the floor

static IRsend irsend(IR_PIN);

static void ts() { Serial.printf("[%02u:%02u.%03u] ",
    (unsigned)(millis()/60000), (unsigned)((millis()%60000)/1000), (unsigned)(millis()%1000)); }

static void irOnce(uint32_t code) { irsend.sendNEC(code, 32); }
static void irFast(uint32_t code) {
    irsend.sendGeneric(kNecHdrMark, kNecHdrSpace, kNecBitMark, kNecOneSpace,
                       kNecBitMark, kNecZeroSpace, kNecBitMark, FAST_GAP_US, 0,
                       code, 32, 38, true, 0, 33);
}

// ── effects ─────────────────────────────────────────────────────────────────
enum Effect : uint8_t { FX_NONE, FX_SWAMP, FX_RED, FX_ZOMBIE };
static const char* fxName(Effect e) {
    return e == FX_SWAMP ? "SWAMP" : e == FX_RED ? "RED" : e == FX_ZOMBIE ? "ZOMBIE" : "NONE";
}
static Effect s_fx = FX_NONE;

// Brightness "breath" fade: [color x2] UP*steps, hold, DOWN*(steps+extra), hold.
struct Fade {
    uint32_t main, accent;
    bool     useAccent;
    uint16_t steps, stepMs, mainCycles;
};
static const Fade FADE_SWAMP = { IR_GREEN, IR_AQUA, true, 4, 0, 3 };     // Joe-approved 09-30
static const Fade FADE_RED   = { IR_RED,   0,       false, 10, 60, 1 };  // Joe-approved 09-30

static Fade     s_fade;
static uint16_t s_fadeCycle = 0;
static int32_t  s_fadePos = 0;
static uint32_t s_nextAt = 0;

static void fadeStart(const Fade& f) {
    s_fade = f;
    irOnce(f.main); delay(40); irOnce(f.main);          // color first: visible at once
    for (int i = 0; i < f.steps + FLOOR_EXTRA; i++) { irFast(IR_BRIGHT_DN); delay(30); }
    s_fadeCycle = 0; s_fadePos = 0; s_nextAt = millis() + 200;
}

static void fadeTick() {
    if ((int32_t)(millis() - s_nextAt) < 0) return;
    bool accent = s_fade.useAccent && (s_fadeCycle == s_fade.mainCycles);
    uint16_t stepMs = accent ? s_fade.stepMs / 2 : s_fade.stepMs;
    int32_t up = s_fade.steps, down = s_fade.steps + FLOOR_EXTRA;
    int32_t p = s_fadePos++;
    uint32_t wait = stepMs;
    if (p == 0) {                                         // at the floor: (re)assert color
        uint32_t c = accent ? s_fade.accent : s_fade.main;
        irOnce(c); delay(40); irOnce(c);
    } else if (p <= up) {
        irFast(IR_BRIGHT_UP);
        if (p == up) wait = stepMs * 3;
    } else if (p <= up + down) {
        irFast(IR_BRIGHT_DN);
        if (p == up + down) {
            wait = stepMs * 2;
            s_fadePos = 0;
            s_fadeCycle = (s_fadeCycle + 1) % (s_fade.mainCycles + (s_fade.useAccent ? 1 : 0));
        }
    }
    s_nextAt = millis() + wait;
}

// Zombie: DIY1 dark, random bursts of the controller's own white FLASH strobe.
// Burst setup = the bench-proven W sequence with its original spacing. Tighter
// spacing made the controller drop FLASH (solid white). ~2.6 s, blocking;
// delay() yields to the WiFi stack.
static const uint32_t Z_DARK_MIN = 6000, Z_DARK_MAX = 15000;    // Joe 09-30: "extend the DIY1"
static const uint32_t Z_BURST_MIN = 1500, Z_BURST_MAX = 4000;
static bool s_zLit = false;

static void whiteStrobeSetup() {
    irOnce(IR_WHITE); delay(150); irOnce(IR_WHITE); delay(150);
    for (int i = 0; i < 10; i++) irFast(IR_BRIGHT_UP);
    delay(150);
    irOnce(IR_FLASH); delay(150); irOnce(IR_FLASH); delay(300);
    for (int i = 0; i < 10; i++) { irOnce(IR_QUICK); delay(60); }
}
static void zombieDark() { irOnce(IR_DARK_DIY1); delay(30); irOnce(IR_DARK_DIY1); }

static void zombieStart() {
    // Glass break: strobe immediately (the break moment IS strobe blip #1).
    whiteStrobeSetup();
    s_zLit = true;
    s_nextAt = millis() + random(Z_BURST_MIN, Z_BURST_MAX + 1);
}

static void zombieTick() {
    if ((int32_t)(millis() - s_nextAt) < 0) return;
    uint32_t dwell;
    if (s_zLit) { zombieDark(); dwell = random(Z_DARK_MIN, Z_DARK_MAX + 1); }
    else        { whiteStrobeSetup(); dwell = random(Z_BURST_MIN, Z_BURST_MAX + 1); }
    s_zLit = !s_zLit;
    s_nextAt = millis() + dwell;
}

static void setEffect(Effect e, const char* why) {
    if (e == s_fx) return;
    ts(); Serial.printf("[fx] %s -> %s (%s)\n", fxName(s_fx), fxName(e), why);
    s_fx = e;
    switch (e) {
        case FX_SWAMP:  fadeStart(FADE_SWAMP); break;
        case FX_RED:    fadeStart(FADE_RED);   break;
        case FX_ZOMBIE: zombieStart();         break;
        default: break;
    }
}

static void fxTick() {
    switch (s_fx) {
        case FX_SWAMP: case FX_RED: fadeTick(); break;
        case FX_ZOMBIE: zombieTick(); break;
        default: break;
    }
}

// ── show director ───────────────────────────────────────────────────────────
static bool     s_showOn = false;
static uint32_t s_showT0 = 0;
static uint32_t s_lastFiredSession = 0;
static uint32_t s_showsRun = 0, s_showsAborted = 0;

static void showStart(uint32_t offsetMs, const char* why) {
    s_showOn = true;
    s_showT0 = millis() - offsetMs;
    s_showsRun++;
    ts(); Serial.printf("[show] START (%s) offset %lu ms\n", why, (unsigned long)offsetMs);
}

static void showStop(const char* why, bool aborted) {
    if (!s_showOn) return;
    s_showOn = false;
    if (aborted) s_showsAborted++;
    ts(); Serial.printf("[show] END (%s) at %lu ms\n", why, (unsigned long)(millis() - s_showT0));
    setEffect(FX_SWAMP, why);
}

static void directorTick() {
    if (!s_showOn) { setEffect(FX_SWAMP, "idle"); return; }
    uint32_t el = millis() - s_showT0;
    if      (el >= SHOW_MAX_MS)      showStop("5 min hard cap (deadman)", true);
    else if (el >= T_RESET_MS)       showStop("clock: 2:49.86 reset (deadman)", false);
    else if (el >= T_GLASS_BREAK_MS) setEffect(FX_ZOMBIE, "1:20.10 glass break");
    else if (el >= T_RED_START_MS)   setEffect(FX_RED, "1:13.40 warning");
    else                             setEffect(FX_SWAMP, "contained");
}

// ── ESP-NOW receive: minimal, flags only, no Serial/IR in the callback ──────
enum RxEvent : uint8_t { RX_NONE = 0, RX_START_ROTLD, RX_START_TOUCHPAD, RX_END, RX_RESET,
                         RX_BENCH_START, RX_BENCH_END, RX_BENCH_OFF };
static volatile uint8_t  s_rxEvent = RX_NONE;
static volatile uint32_t s_rxSession = 0, s_rxArg = 0;
static volatile uint32_t s_rxFrames = 0, s_rxLastMs = 0;

static void onDataRecv(uint8_t* /*mac*/, uint8_t* data, uint8_t len) {
    if (len == (uint8_t)sizeof(BenchFrame)) {
        BenchFrame f; memcpy(&f, data, sizeof(f));
        if (!bench_valid(f)) return;
        s_rxArg = f.arg;
        s_rxEvent = f.cmd == BENCH_START ? RX_BENCH_START
                  : f.cmd == BENCH_END   ? RX_BENCH_END
                  : f.cmd == BENCH_OFF   ? RX_BENCH_OFF : RX_NONE;
        return;
    }
    if (len != (uint8_t)sizeof(Msg)) return;
    Msg m; memcpy(&m, data, sizeof(Msg));
    if (!msg_valid(m)) return;
    s_rxFrames++; s_rxLastMs = millis();
    if (m.target != (uint8_t)NODE_BROADCAST && m.target != (uint8_t)THIS_NODE) return;

    if (m.command == (uint8_t)CMD_SET_THEME && m.payload_a == (uint32_t)THEME_ROTLD) {
        s_rxSession = m.session_id; s_rxEvent = RX_START_ROTLD;
    } else if (m.command == (uint8_t)CMD_SEGMENT_START && m.payload_a == (uint32_t)SEGMENT_ZOMBIE) {
        s_rxSession = m.session_id; s_rxEvent = RX_START_TOUCHPAD;
    } else if (m.command == (uint8_t)CMD_SEGMENT_END && m.payload_a == (uint32_t)SEGMENT_ZOMBIE) {
        s_rxEvent = RX_END;
    } else if (m.command == (uint8_t)CMD_SHOW_STATE &&
               (m.payload_a == (uint32_t)SHOW_IDLE || m.payload_a == (uint32_t)SHOW_OFF)) {
        s_rxEvent = RX_RESET;
    }
}

static void handleRx() {
    uint8_t ev = s_rxEvent;
    if (ev == RX_NONE) return;
    s_rxEvent = RX_NONE;
    switch (ev) {
        case RX_START_ROTLD:
        case RX_START_TOUCHPAD: {
            // Same dedupe as the relay: Scarecrow's theme burst and the
            // touchpad's fire share a session_id and each arrive 3x.
            uint32_t session = s_rxSession;
            if (session != 0 && session == s_lastFiredSession) return;
            if (s_showOn) {   // lockout: a run in progress is never restarted
                ts(); Serial.printf("[show] start 0x%08lX ignored, show already running (%lu ms)\n",
                                    (unsigned long)session, (unsigned long)(millis() - s_showT0));
                return;
            }
            s_lastFiredSession = session;
            showStart(0, ev == RX_START_ROTLD ? "SET_THEME ROTLD" : "touchpad SEGMENT_START");
            break;
        }
        case RX_END:          showStop("SEGMENT_END", true); break;
        case RX_RESET:        showStop("SHOW_STATE idle/off", true); break;
        case RX_BENCH_START:  showStart(s_rxArg, "BENCH"); break;
        case RX_BENCH_END:    showStop("BENCH end", true); break;
        case RX_BENCH_OFF:    showStop("BENCH off", true); break;
    }
}

// ── setup / loop ────────────────────────────────────────────────────────────
void setup() {
    delay(3000);   // power-rail settle before WiFi, same as the relay / area-lights
    Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);   // GPIO3 belongs to the IR LED
    delay(100);
    irsend.begin();
    uint8_t mac[6]; wifi_get_macaddr(STATION_IF, mac);
    Serial.printf("\n[zombie-ir] boot, node 0x%02X, MAC %02X:%02X:%02X:%02X:%02X:%02X, reset: %s\n",
                  (unsigned)THIS_NODE, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  ESP.getResetReason().c_str());
    randomSeed(RANDOM_REG32);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    wifi_set_channel(WIFI_CHANNEL);
    if (esp_now_init() != 0) {
        Serial.println(F("[espnow] init FAILED, rebooting in 3s (lights stay on last state)"));
        delay(3000);
        ESP.restart();
    }
    esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
    esp_now_register_recv_cb(onDataRecv);
    Serial.printf("[espnow] listening on channel %u\n", WIFI_CHANNEL);

    setEffect(FX_SWAMP, "boot");
}

void loop() {
    handleRx();
    directorTick();
    fxTick();

    if (!s_showOn && s_fx == FX_SWAMP && millis() >= IDLE_REBOOT_MS) {
        ts(); Serial.println(F("[health] daily preventive restart (idle, swamp)"));
        delay(100);
        ESP.restart();
    }

    // Channel drift guard (NETWORK_CONSTRAINTS C1): re-lock to ch 11.
    static uint32_t lastChk = 0;
    if (millis() - lastChk >= 30000) {
        lastChk = millis();
        uint8_t ch = wifi_get_channel();
        if (ch != WIFI_CHANNEL) {
            ts(); Serial.printf("[espnow] channel drift %u -> re-lock %u\n", ch, WIFI_CHANNEL);
            wifi_set_channel(WIFI_CHANNEL);
        }
        ts(); Serial.printf("[status] fx=%s show=%d el=%lu shows=%lu aborted=%lu fleetFrames=%lu lastFrame=%lus ago heap=%u\n",
                            fxName(s_fx), s_showOn,
                            (unsigned long)(s_showOn ? millis() - s_showT0 : 0),
                            (unsigned long)s_showsRun, (unsigned long)s_showsAborted,
                            (unsigned long)s_rxFrames,
                            (unsigned long)(s_rxFrames ? (millis() - s_rxLastMs) / 1000 : 0),
                            ESP.getFreeHeap());
    }
}
