// =============================================================================
// Zombie Container DMX light: SHOW firmware (ESP32 DevKit -> RS485 -> par can)
// =============================================================================
// Follows the Zombie Container's PicoDMX show over ESP-NOW (listen-only):
//   all night (idle)        SWAMP      smooth green breathing + aqua swell
//   show 0:00  -> 1:13.40   SWAMP      CONTAINED
//   show 1:13.40 -> 1:20.10 HEARTBEAT  WARNING: red lub-dub, 70 bpm
//   show 1:20.10 -> 2:49.86 ZOMBIE     glass break -> shuffled white spark bursts
//   show 2:49.86 ->         SWAMP      reset, back to the night loop
// Boundaries = docs/zombie-container-dmx-timeline.md, same as
// props/touchpad-ota-controller/src/dmx_timeline.h + props/zombie-ir-lights.
// Effects approved by Joe 2026-09-30 on the bench (props/zombie-dmx-light
// env dmx_probe: `swamp`, `heart`, `zstrobe`).
//
// TRIGGER: identical to props/zombie-relay-esp01s (the board that fires the
// PicoDMX): Scarecrow CMD_SET_THEME=THEME_ROTLD or touchpad
// CMD_SEGMENT_START=SEGMENT_ZOMBIE, one dedupe per session_id.
//
// FAILSAFES (nothing reports back; every path lands on SWAMP):
//   - Local show clock -> SWAMP at 2:49.86 (the touchpad sends no END when the
//     show finishes normally).
//   - 5 min hard cap on any show state, independent of the clock logic.
//   - Start lockout while a show runs (repeat starts can't extend it).
//   - CMD_SEGMENT_END / CMD_SHOW_STATE IDLE|OFF -> SWAMP now.
//   - Missed start -> stays SWAMP.
//   - Task watchdog (5 s): a hung loop reboots. Most fixtures HOLD the last DMX
//     frame when the signal stops, so a hang mid-flash could leave it white.
//     Boot sends SWAMP within ~1 s.
//   - Daily preventive restart while idle. Channel re-lock to 11 every 30 s.
//   - DMX is streamed continuously (~44 Hz), never "send once".
//
// Updates + logs: USB via yard-bridge-pi (Joe: that path works; no OTA).
//
// WIRING (bench-proven 2026-09-30):
//   ESP32 TX2 (GPIO17) -> converter **TXD** (labels are MCU-side: TX->TX;
//   the DIYables tutorial's TX->RXD is WRONG for this board)
//   converter RXD -> NOT connected (it's the module's output: 5 V at VCC 5 V)
//   converter VCC -> VIN 5 V (MAX485 is a 5 V part), GND -> GND
//   XLR: pin1 black -> converter ground terminal, pin2 red -> B-, pin3 yellow -> A+
//
// FIXTURE: 7-ch par, menu d.xxx = DMX + start address. Map from the start:
//   +0 master, +1 R, +2 G, +3 B, +4 W, +5 mode (0 manual), +6 strobe/speed.
//   Start address is stored in NVS (serial `base <n>`), default FIXTURE_BASE.
//
// Serial (115200), for the bench / bridge:
//   status | start [sec] | end | base <addr> | dim [1-255] | fx swamp|heart|zombie|off
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <esp_dmx.h>
#include "ProjectConstants.h"
#include "wifi_credentials.h"
#include "segment_protocol.h"

#ifndef FIXTURE_BASE
#define FIXTURE_BASE 1
#endif

// ── timeline, ms from the trigger ───────────────────────────────────────────
static const uint32_t T_RED_START_MS   =  73400;   // 1:13.40 WARNING
static const uint32_t T_GLASS_BREAK_MS =  80100;   // 1:20.10 strobe #1 / air cannon
static const uint32_t T_RESET_MS       = 169860;   // 2:49.86 green returns
static const uint32_t SHOW_MAX_MS      = 300000;   // 5 min hard cap
static const uint32_t IDLE_REBOOT_MS   = 24UL * 3600UL * 1000UL;
static const int      WDT_TIMEOUT_S    = 5;

// ── DMX ─────────────────────────────────────────────────────────────────────
static const dmx_port_t kDmxPort = DMX_NUM_1;      // (DMX_NUM_2 crashes on this core: esp_dmx uart.c)
static const int kTxPin = 17, kRxPin = 16;
static uint8_t universe[DMX_PACKET_SIZE];
static int fixtureBase = FIXTURE_BASE;
static uint32_t framesSent = 0;
static Preferences prefs;

static void ts() { uint32_t m = millis();
    Serial.printf("[%02lu:%02lu:%02lu.%03lu] ", (unsigned long)(m / 3600000), (unsigned long)(m / 60000 % 60),
                  (unsigned long)(m / 1000 % 60), (unsigned long)(m % 1000)); }

static uint8_t* fx() { return &universe[fixtureBase]; }
static void fixtureSet(uint8_t master, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    uint8_t* f = fx();
    f[0] = master; f[1] = r; f[2] = g; f[3] = b; f[4] = w;
    f[5] = 0;            // manual mode, never a built-in program
    f[6] = 0;            // fixture strobe NEVER used (flashes are ESP-made, rate-limited)
}

// ── effect: SWAMP (Joe-approved "good mixture") ─────────────────────────────
static const uint32_t SWAMP_BREATH_MS = 5000;
static const uint8_t  SWAMP_FLOOR = 40;
// Swamp master dimmer (Joe 2026-10-02: full 255 was "like daylight"; 60/40/10 % tried). Swamp only;
// heartbeat + zombie flashes keep full punch. NVS "swamp_dim", serial `dim <n>`.
static const uint8_t  SWAMP_DIM_DEFAULT = 51;   // 20 %, Joe-approved 2026-10-02 ("perfect")
static uint8_t swampDim = SWAMP_DIM_DEFAULT;
static uint32_t swampT0 = 0;
static float ripple = 0, rippleTarget = 0;
static uint32_t rippleNextAt = 0;

static void swampTick(uint32_t now) {
    uint32_t aquaMs = SWAMP_BREATH_MS * 7 / 10;
    uint32_t cycle = SWAMP_BREATH_MS * 3 + aquaMs;
    uint32_t t = (now - swampT0) % cycle;
    bool aqua = t >= SWAMP_BREATH_MS * 3;
    float ph = aqua ? (float)(t - SWAMP_BREATH_MS * 3) / aquaMs : (float)(t % SWAMP_BREATH_MS) / SWAMP_BREATH_MS;
    float breath = 0.5f - 0.5f * cosf(ph * 2.0f * PI);
    if ((int32_t)(now - rippleNextAt) >= 0) {
        rippleTarget = random(-100, 101) / 1000.0f;
        rippleNextAt = now + random(300, 1200);
    }
    ripple += (rippleTarget - ripple) * 0.05f;
    float g = (SWAMP_FLOOR + (255 - SWAMP_FLOOR) * breath) * (1.0f + ripple);
    float b = aqua ? 170.0f * breath : 12.0f;
    if (aqua) g = SWAMP_FLOOR + (200 - SWAMP_FLOOR) * (0.6f + 0.4f * breath);
    fixtureSet(swampDim, 0, (uint8_t)constrain((int)g, 0, 255), (uint8_t)constrain((int)b, 0, 255), 0);
}

// ── effect: HEARTBEAT (Joe-approved) ────────────────────────────────────────
static const uint16_t HEART_BPM = 70;
static uint32_t heartT0 = 0;
static float pulse(float t, float start, float amp) {
    float d = t - start;
    if (d < 0) return 0;
    if (d < 45) return amp * (d / 45.0f);
    return amp * expf(-(d - 45) / 110.0f);
}
static void heartTick(uint32_t now) {
    float beatMs = 60000.0f / HEART_BPM;
    float t = fmodf((float)(now - heartT0), beatMs);
    float e = pulse(t, 0, 1.0f) + pulse(t, 260, 0.7f);
    if (e > 1.0f) e = 1.0f;
    fixtureSet(255, (uint8_t)(255 * e), 0, 0, 0);
}

// ── effect: ZOMBIE strobe (Joe-approved at the photosensitive-safe rate) ────
// Fisher-Yates decks: bursts 0.2-4 s, black < 10 s. Inside a burst: 50 ms
// white flashes, 350-600 ms apart (<= 2.9 flashes/s, WCAG/Ofcom <= 3/s).
static const uint16_t kBurstDeck[] = {200, 500, 800, 1200, 1600, 2000, 2500, 3000, 3500, 4000};
static const uint16_t kGapDeck[]   = {500, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 9500};
static const uint16_t kFlashOnMs = 50, kFlashPeriodMinMs = 350, kFlashPeriodMaxMs = 600;
static const uint8_t  kZombieLevel = 140;

struct Deck {
    const uint16_t* src; uint8_t n; uint16_t order[12]; uint8_t pos; uint16_t last;
    void shuffle() {
        for (uint8_t i = 0; i < n; i++) order[i] = src[i];
        for (int i = n - 1; i > 0; i--) { int j = esp_random() % (i + 1); uint16_t t = order[i]; order[i] = order[j]; order[j] = t; }
        if (order[0] == last && n > 1) { uint16_t t = order[0]; order[0] = order[1]; order[1] = t; }
        pos = 0;
    }
    uint16_t next() { if (pos >= n) shuffle(); last = order[pos++]; return last; }
};
static Deck burstDeck = {kBurstDeck, sizeof(kBurstDeck) / 2, {}, 255, 0};
static Deck gapDeck   = {kGapDeck,   sizeof(kGapDeck) / 2,   {}, 255, 0};
static bool zInBurst = false, zFlashLit = false;
static uint32_t zPhaseEnd = 0, zFlashNext = 0;

static void zombieStart(uint32_t now) {
    // The glass break IS strobe blip #1: open with a burst immediately.
    zInBurst = true; zFlashLit = false; zFlashNext = now;
    zPhaseEnd = now + burstDeck.next();
}
static void zombieTick(uint32_t now) {
    if ((int32_t)(now - zPhaseEnd) >= 0) {
        zInBurst = !zInBurst;
        zPhaseEnd = now + (zInBurst ? burstDeck.next() : gapDeck.next());
        zFlashLit = false; zFlashNext = now;
    }
    if (zInBurst && (int32_t)(now - zFlashNext) >= 0) {
        zFlashLit = !zFlashLit;
        zFlashNext = now + (zFlashLit ? kFlashOnMs
                                      : (uint32_t)random(kFlashPeriodMinMs, kFlashPeriodMaxMs + 1) - kFlashOnMs);
    }
    bool lit = zInBurst && zFlashLit;
    fixtureSet(lit ? kZombieLevel : 0, 0, 0, 0, lit ? 255 : 0);
}

// ── effect selection ────────────────────────────────────────────────────────
enum Effect : uint8_t { FX_NONE, FX_SWAMP, FX_HEART, FX_ZOMBIE, FX_OFF };
static const char* fxName(Effect e) {
    switch (e) { case FX_SWAMP: return "SWAMP"; case FX_HEART: return "HEARTBEAT";
                 case FX_ZOMBIE: return "ZOMBIE"; case FX_OFF: return "OFF"; default: return "NONE"; }
}
static Effect s_fx = FX_NONE;
static bool s_manualFx = false;          // bench `fx` override (cleared by start/end/reset)

static void setEffect(Effect e, const char* why) {
    if (e == s_fx) return;
    ts(); Serial.printf("[fx] %s -> %s (%s)\n", fxName(s_fx), fxName(e), why);
    s_fx = e;
    uint32_t now = millis();
    if (e == FX_SWAMP) swampT0 = now;
    if (e == FX_HEART) heartT0 = now;
    if (e == FX_ZOMBIE) zombieStart(now);
}
static void fxTick() {
    uint32_t now = millis();
    switch (s_fx) {
        case FX_SWAMP:  swampTick(now); break;
        case FX_HEART:  heartTick(now); break;
        case FX_ZOMBIE: zombieTick(now); break;
        default:        fixtureSet(0, 0, 0, 0, 0); break;
    }
}

// ── show director ───────────────────────────────────────────────────────────
static bool s_showOn = false;
static uint32_t s_showT0 = 0, s_lastFiredSession = 0, s_shows = 0, s_aborted = 0;

static void showStart(uint32_t offsetMs, const char* why) {
    s_showOn = true; s_manualFx = false;
    s_showT0 = millis() - offsetMs;
    s_shows++;
    ts(); Serial.printf("[show] START (%s) offset %lu ms\n", why, (unsigned long)offsetMs);
}
static void showStop(const char* why, bool aborted) {
    s_manualFx = false;
    if (!s_showOn) { setEffect(FX_SWAMP, why); return; }
    s_showOn = false;
    if (aborted) s_aborted++;
    ts(); Serial.printf("[show] END (%s) at %lu ms\n", why, (unsigned long)(millis() - s_showT0));
    setEffect(FX_SWAMP, why);
}
static void directorTick() {
    if (s_manualFx) return;
    if (!s_showOn) { setEffect(FX_SWAMP, "idle"); return; }
    uint32_t el = millis() - s_showT0;
    if      (el >= SHOW_MAX_MS)      showStop("5 min hard cap (deadman)", true);
    else if (el >= T_RESET_MS)       showStop("clock: 2:49.86 reset (deadman)", false);
    else if (el >= T_GLASS_BREAK_MS) setEffect(FX_ZOMBIE, "1:20.10 glass break");
    else if (el >= T_RED_START_MS)   setEffect(FX_HEART, "1:13.40 warning");
    else                             setEffect(FX_SWAMP, "contained");
}

// ── ESP-NOW (listen-only). Callback -> queue; handled in loop() ─────────────
enum RxEv : uint8_t { RX_START_ROTLD = 1, RX_START_TOUCHPAD, RX_END, RX_RESET };
struct RxItem { uint8_t ev; uint32_t session; };
static QueueHandle_t s_rxQ;
static volatile uint32_t s_rxFrames = 0, s_rxLastMs = 0, s_rxDropped = 0;

static void handleFrame(const uint8_t* data, int len) {
    if (len != (int)sizeof(Msg)) return;
    Msg m; memcpy(&m, data, sizeof(Msg));
    if (!msg_valid(m)) return;
    s_rxFrames++; s_rxLastMs = millis();
    if (m.target != (uint8_t)NODE_BROADCAST && m.target != (uint8_t)THIS_NODE) return;
    RxItem it = {0, m.session_id};
    if (m.command == CMD_SET_THEME && m.payload_a == THEME_ROTLD)                 it.ev = RX_START_ROTLD;
    else if (m.command == CMD_SEGMENT_START && m.payload_a == SEGMENT_ZOMBIE)     it.ev = RX_START_TOUCHPAD;
    else if (m.command == CMD_SEGMENT_END && m.payload_a == SEGMENT_ZOMBIE)       it.ev = RX_END;
    else if (m.command == CMD_SHOW_STATE && (m.payload_a == SHOW_IDLE || m.payload_a == SHOW_OFF)) it.ev = RX_RESET;
    else return;
    if (xQueueSend(s_rxQ, &it, 0) != pdTRUE) s_rxDropped++;
}
#if ESP_IDF_VERSION_MAJOR >= 5
static void onRecv(const esp_now_recv_info_t*, const uint8_t* d, int len) { handleFrame(d, len); }
#else
static void onRecv(const uint8_t*, const uint8_t* d, int len) { handleFrame(d, len); }
#endif

static void handleRx() {
    RxItem it;
    while (xQueueReceive(s_rxQ, &it, 0) == pdTRUE) {
        switch (it.ev) {
            case RX_START_ROTLD:
            case RX_START_TOUCHPAD:
                if (it.session != 0 && it.session == s_lastFiredSession) break;   // 3x bursts + shared session
                if (s_showOn) {
                    ts(); Serial.printf("[show] start 0x%08lX ignored, show running (%lu ms)\n",
                                        (unsigned long)it.session, (unsigned long)(millis() - s_showT0));
                    break;
                }
                s_lastFiredSession = it.session;
                showStart(0, it.ev == RX_START_ROTLD ? "SET_THEME ROTLD" : "touchpad SEGMENT_START");
                break;
            case RX_END:   showStop("SEGMENT_END", true); break;
            case RX_RESET: showStop("SHOW_STATE idle/off", true); break;
        }
    }
}

// ── serial (bench / yard-bridge-pi) ───────────────────────────────────────
static void printStatus() {
    ts(); Serial.printf("[status] fx=%s show=%d el=%lu shows=%lu aborted=%lu base=%d dim=%u frames=%lu fleetFrames=%lu lastFrame=%lus ago rxDropped=%lu heap=%u\n",
                        fxName(s_fx), s_showOn, (unsigned long)(s_showOn ? millis() - s_showT0 : 0),
                        (unsigned long)s_shows, (unsigned long)s_aborted, fixtureBase, (unsigned)swampDim, (unsigned long)framesSent,
                        (unsigned long)s_rxFrames, (unsigned long)(s_rxFrames ? (millis() - s_rxLastMs) / 1000 : 0),
                        (unsigned long)s_rxDropped, ESP.getFreeHeap());
}
static void handleSerial() {
    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c != '\n' && c != '\r') { if (line.length() < 64) line += c; continue; }
        line.trim();
        if (line == "status") printStatus();
        else if (line.startsWith("start")) {   // bench: start [sec] (bypasses the lockout)
            uint32_t sec = line.length() > 5 ? line.substring(6).toInt() : 0;
            showStart(sec * 1000, "SERIAL bench");
        }
        else if (line == "end") showStop("SERIAL end", true);
        else if (line.startsWith("base ")) {
            int b = line.substring(5).toInt();
            if (b >= 1 && b <= 506) {
                memset(universe + 1, 0, DMX_PACKET_SIZE - 1);
                fixtureBase = b; prefs.putInt("base", b);
                ts(); Serial.printf("[cfg] fixture base = %d (saved)\n", b);
            }
        }
        else if (line == "dim") { ts(); Serial.printf("[cfg] swamp dim = %u/255\n", swampDim); }
        else if (line.startsWith("dim ")) {   // live: takes effect on the next swamp frame
            int d = line.substring(4).toInt();
            if (d >= 1 && d <= 255) {
                swampDim = (uint8_t)d; prefs.putUChar("swamp_dim", swampDim);
                ts(); Serial.printf("[cfg] swamp dim = %d/255 (saved)\n", d);
            } else { ts(); Serial.println("[cfg] dim must be 1-255 (unchanged)"); }
        }
        else if (line.startsWith("fx ")) {    // bench: preview one effect (no show clock)
            String n = line.substring(3);
            Effect e = n == "swamp" ? FX_SWAMP : n == "heart" ? FX_HEART : n == "zombie" ? FX_ZOMBIE : FX_OFF;
            s_showOn = false; s_manualFx = true;
            setEffect(e, "SERIAL fx");
        }
        else if (line.length()) { ts(); Serial.printf("[cmd] ? '%s' (status|start [s]|end|base n|dim [n]|fx swamp|heart|zombie|off)\n", line.c_str()); }
        line = "";
    }
}

// ── setup / loop ────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(100);

    // DMX first, so the fixture gets SWAMP as early as possible after any reset.
    memset(universe, 0, sizeof(universe));
    prefs.begin("zdmx", false);
    fixtureBase = prefs.getInt("base", FIXTURE_BASE);
    swampDim = prefs.getUChar("swamp_dim", SWAMP_DIM_DEFAULT);
    if (swampDim == 0) swampDim = SWAMP_DIM_DEFAULT;
    dmx_config_t config = DMX_CONFIG_DEFAULT;
    dmx_personality_t personalities[] = {};
    dmx_driver_install(kDmxPort, &config, personalities, 0);
    dmx_set_pin(kDmxPort, kTxPin, kRxPin, DMX_PIN_NO_CHANGE);
    setEffect(FX_SWAMP, "boot");

    ts(); Serial.printf("[zombie-dmx] boot, node 0x%02X, MAC %s, reset reason %d, fixture base %d\n",
                        (unsigned)THIS_NODE, WiFi.macAddress().c_str(), (int)esp_reset_reason(), fixtureBase);

    s_rxQ = xQueueCreate(16, sizeof(RxItem));
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) {
        ts(); Serial.println("[espnow] init FAILED, rebooting in 3 s");
        delay(3000); ESP.restart();
    }
    esp_now_register_recv_cb(onRecv);
    ts(); Serial.printf("[espnow] listening on channel %d\n", WIFI_CHANNEL);

    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);
}

void loop() {
    esp_task_wdt_reset();
    handleSerial();
    handleRx();
    directorTick();
    fxTick();

    // Continuous DMX stream (~44 Hz). Fixtures hold the last frame on signal loss.
    dmx_write(kDmxPort, universe, DMX_PACKET_SIZE);
    dmx_send(kDmxPort);
    dmx_wait_sent(kDmxPort, DMX_TIMEOUT_TICK);
    framesSent++;

    if (!s_showOn && s_fx == FX_SWAMP && millis() >= IDLE_REBOOT_MS) {
        ts(); Serial.println("[health] daily preventive restart (idle, swamp)");
        delay(100); ESP.restart();
    }
    static uint32_t lastChk = 0;
    if (millis() - lastChk >= 30000) {
        lastChk = millis();
        uint8_t ch; wifi_second_chan_t sc;
        if (esp_wifi_get_channel(&ch, &sc) == ESP_OK && ch != WIFI_CHANNEL) {
            ts(); Serial.printf("[espnow] channel drift %u -> re-lock %d\n", ch, WIFI_CHANNEL);
            esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
        }
        printStatus();
    }
}
