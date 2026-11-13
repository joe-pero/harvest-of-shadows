// =============================================================================
// Parkway MOSFET lights: SHOW firmware (ESP32 DevKit -> 4 single MOSFET modules)
// =============================================================================
// Follows the Zombie Container's show over ESP-NOW (listen-only), same timeline
// and trigger as props/zombie-dmx-light (this file is that firmware with the DMX
// output swapped for 4 PWM channels):
//   all night (idle)        SWAMP   green breathing + aqua swell (Halloween loop)
//   show 0:00  -> 1:13.40   SWAMP   CONTAINED
//   show 1:13.40 -> 1:20.10 ALERT   red alert: red lub-dub, 70 bpm
//   show 1:20.10 -> 2:49.86 STROBE  "white" strobe on the AMBER channel (Joe 10-07),
//                                   shuffled bursts, <= 2.9 flashes/s
//   show 2:49.86 ->         SWAMP   back to the night loop
// Boundaries = docs/zombie-container-dmx-timeline.md.
//
// TRIGGER: identical to props/zombie-relay-esp01s: Scarecrow CMD_SET_THEME=
// THEME_ROTLD or touchpad CMD_SEGMENT_START=SEGMENT_ZOMBIE, one dedupe per session_id.
//
// FAILSAFES (nothing reports back; every path lands on SWAMP):
//   local show clock -> SWAMP at 2:49.86, 5 min hard cap, start lockout while a
//   show runs, SEGMENT_END / SHOW_STATE IDLE|OFF -> SWAMP, missed start -> stays
//   SWAMP, task watchdog 5 s, all outputs LOW before anything else at boot,
//   daily idle restart, channel re-lock to 11 every 30 s.
//
// CHANNELS (probed 2026-10-07 by Joe's eye, see platformio.ini):
//   GPIO25 = RED, GPIO26 = GREEN, GPIO27 = AQUA BLUE, GPIO33 = AMBER
// POWER: never PC USB + the AC/DC converter on VIN at the same time (that combo
// cost a 4-ch MOSFET board + an ESP32). USB bench = converter off VIN.
//
// Serial (115200), bench:
//   status | start [sec] | end | dim [1-255] | fx swamp|alert|strobe|off
//   nextboot [sec]: run the show ONCE at the next boot from <sec> (default 63), for off-USB checks
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include "ProjectConstants.h"
#include "wifi_credentials.h"
#include "segment_protocol.h"

// ── timeline, ms from the trigger (same as zombie-dmx-light) ────────────────
static const uint32_t T_RED_START_MS   =  73400;   // 1:13.40 WARNING
static const uint32_t T_GLASS_BREAK_MS =  80100;   // 1:20.10 strobe #1 / air cannon
static const uint32_t T_RESET_MS       = 169860;   // 2:49.86 green returns
static const uint32_t SHOW_MAX_MS      = 300000;   // 5 min hard cap
static const uint32_t IDLE_REBOOT_MS   = 24UL * 3600UL * 1000UL;
static const int      WDT_TIMEOUT_S    = 5;

// ── outputs: 4 LEDC PWM channels ────────────────────────────────────────────
enum Ch : uint8_t { CH_RED, CH_GREEN, CH_BLUE, CH_AMBER };
static const uint8_t  kPins[4]  = {25, 26, 27, 33};
static const uint32_t kPwmHz    = 1000;      // Ceksezx modules: 0-20 kHz
static const uint8_t  kPwmBits  = 12;
static uint8_t out[4] = {0, 0, 0, 0};        // last 0-255 level per channel (pre-gamma)
static uint16_t gammaLut[256];
static Preferences prefs;

static void ts() { uint32_t m = millis();
    Serial.printf("[%02lu:%02lu:%02lu.%03lu] ", (unsigned long)(m / 3600000), (unsigned long)(m / 60000 % 60),
                  (unsigned long)(m / 1000 % 60), (unsigned long)(m % 1000)); }

// Raw MOSFET PWM is linear in power, which the eye sees as "jumps to bright then
// flat". Gamma 2.2 makes the breathing fades look smooth, esp. at the low end.
// Levels below kMinOn snap to true 0 (Joe 10-07: faint low levels look wrong on
// these lights). A channel at 0 is detached from PWM and held hard LOW.
static const uint8_t kMinOn = 20;
static void lightsSet(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t v[4] = {r, g, b, a};
    for (int i = 0; i < 4; i++) {
        if (v[i] < kMinOn) v[i] = 0;
        if (v[i] == out[i]) continue;
        if (v[i] == 0) {
            ledcDetachPin(kPins[i]); pinMode(kPins[i], OUTPUT); digitalWrite(kPins[i], LOW);
        } else {
            if (out[i] == 0) ledcAttachPin(kPins[i], i);
            ledcWrite(i, gammaLut[v[i]]);
        }
        out[i] = v[i];
    }
}
static uint8_t scale(float v, uint8_t dim) { return (uint8_t)constrain((int)(v * dim / 255.0f), 0, 255); }

// ── effect: SWAMP (zombie-dmx-light's Joe-approved swamp, on R/G/B) ─────────
static const uint32_t SWAMP_BREATH_MS = 5000;
static const uint8_t  SWAMP_FLOOR = 40;
static const uint8_t  SWAMP_DIM_DEFAULT = 128;  // 50 %: starting point, tune by eye with `dim` (NVS "swamp_dim")
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
    // Blue is on/off only (Joe 10-07: this aqua LED looks bad at low levels):
    // off for the green breaths, straight on for the aqua swell.
    if (aqua) g = SWAMP_FLOOR + (200 - SWAMP_FLOOR) * (0.6f + 0.4f * breath);
    uint8_t b = aqua ? scale(170.0f, swampDim) : 0;
    lightsSet(0, scale(g, swampDim), b, 0);
}

// ── effect: RED ALERT (zombie-dmx-light's heartbeat, red channel) ───────────
static const uint16_t ALERT_BPM = 70;
static uint32_t alertT0 = 0;
static float pulse(float t, float start, float amp) {
    float d = t - start;
    if (d < 0) return 0;
    if (d < 45) return amp * (d / 45.0f);
    return amp * expf(-(d - 45) / 110.0f);
}
static void alertTick(uint32_t now) {
    float beatMs = 60000.0f / ALERT_BPM;
    float t = fmodf((float)(now - alertT0), beatMs);
    float e = pulse(t, 0, 1.0f) + pulse(t, 260, 0.7f);
    if (e > 1.0f) e = 1.0f;
    lightsSet((uint8_t)(255 * e), 0, 0, 0);
}

// ── effect: STROBE on AMBER (zombie-dmx-light's photosensitive-safe bursts) ─
// Fisher-Yates decks: bursts 0.2-4 s, black < 10 s. Inside a burst: 50 ms
// flashes, 350-600 ms apart (<= 2.9 flashes/s, WCAG/Ofcom <= 3/s).
static const uint16_t kBurstDeck[] = {200, 500, 800, 1200, 1600, 2000, 2500, 3000, 3500, 4000};
static const uint16_t kGapDeck[]   = {500, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 9500};
static const uint16_t kFlashOnMs = 50, kFlashPeriodMinMs = 350, kFlashPeriodMaxMs = 600;
static const uint8_t  kStrobeLevel = 255;

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

static void strobeStart(uint32_t now) {
    // The glass break IS flash #1: open with a burst immediately.
    zInBurst = true; zFlashLit = false; zFlashNext = now;
    zPhaseEnd = now + burstDeck.next();
}
static void strobeTick(uint32_t now) {
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
    lightsSet(0, 0, 0, (zInBurst && zFlashLit) ? kStrobeLevel : 0);
}

// ── effect selection ────────────────────────────────────────────────────────
enum Effect : uint8_t { FX_NONE, FX_SWAMP, FX_ALERT, FX_STROBE, FX_OFF };
static const char* fxName(Effect e) {
    switch (e) { case FX_SWAMP: return "SWAMP"; case FX_ALERT: return "RED_ALERT";
                 case FX_STROBE: return "STROBE"; case FX_OFF: return "OFF"; default: return "NONE"; }
}
static Effect s_fx = FX_NONE;
static bool s_manualFx = false;          // bench `fx` override (cleared by start/end)
static uint32_t s_manualFxAt = 0;
static const uint32_t MANUAL_FX_MAX_MS = 600000;   // a forgotten `fx` preview reverts to the show after 10 min

static void setEffect(Effect e, const char* why) {
    if (e == s_fx) return;
    ts(); Serial.printf("[fx] %s -> %s (%s)\n", fxName(s_fx), fxName(e), why);
    s_fx = e;
    uint32_t now = millis();
    if (e == FX_SWAMP) swampT0 = now;
    if (e == FX_ALERT) alertT0 = now;
    if (e == FX_STROBE) strobeStart(now);
}
static void fxTick() {
    uint32_t now = millis();
    switch (s_fx) {
        case FX_SWAMP:  swampTick(now); break;
        case FX_ALERT:  alertTick(now); break;
        case FX_STROBE: strobeTick(now); break;
        default:        lightsSet(0, 0, 0, 0); break;
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
    if (s_manualFx && millis() - s_manualFxAt >= MANUAL_FX_MAX_MS) {
        s_manualFx = false;
        ts(); Serial.println("[fx] bench preview expired (10 min) -> show control");
    }
    if (s_manualFx) return;
    if (!s_showOn) { setEffect(FX_SWAMP, "idle"); return; }
    uint32_t el = millis() - s_showT0;
    if      (el >= SHOW_MAX_MS)      showStop("5 min hard cap (deadman)", true);
    else if (el >= T_RESET_MS)       showStop("clock: 2:49.86 reset (deadman)", false);
    else if (el >= T_GLASS_BREAK_MS) setEffect(FX_STROBE, "1:20.10 glass break");
    else if (el >= T_RED_START_MS)   setEffect(FX_ALERT, "1:13.40 warning");
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

// ── serial (bench) ──────────────────────────────────────────────────────────
static void printStatus() {
    ts(); Serial.printf("[status] fx=%s show=%d el=%lu shows=%lu aborted=%lu dim=%u out R%u G%u B%u A%u fleetFrames=%lu lastFrame=%lus ago rxDropped=%lu heap=%u\n",
                        fxName(s_fx), s_showOn, (unsigned long)(s_showOn ? millis() - s_showT0 : 0),
                        (unsigned long)s_shows, (unsigned long)s_aborted, (unsigned)swampDim,
                        out[CH_RED], out[CH_GREEN], out[CH_BLUE], out[CH_AMBER],
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
        else if (line.startsWith("nextboot")) {   // bench: run the show once at the NEXT boot (off-USB check)
            int sec = line.length() > 8 ? line.substring(9).toInt() : 63;   // 63 = ~10 s before red
            if (sec < 0 || sec > 160) sec = 63;
            prefs.putInt("nextboot", sec);
            ts(); Serial.printf("[cfg] next boot runs the show from %d s (one time)\n", sec);
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
            Effect e = n == "swamp" ? FX_SWAMP : n == "alert" ? FX_ALERT : n == "strobe" ? FX_STROBE : FX_OFF;
            s_showOn = false; s_manualFx = true; s_manualFxAt = millis();
            setEffect(e, "SERIAL fx");
        }
        else if (line.length()) { ts(); Serial.printf("[cmd] ? '%s' (status|start [s]|end|dim [n]|fx swamp|alert|strobe|off|nextboot [s])\n", line.c_str()); }
        line = "";
    }
}

// ── setup / loop ────────────────────────────────────────────────────────────
void setup() {
    // Outputs LOW before anything else so nothing flashes at boot.
    for (int i = 0; i < 4; i++) { pinMode(kPins[i], OUTPUT); digitalWrite(kPins[i], LOW); }
    const uint32_t maxDuty = (1u << kPwmBits) - 1;
    for (int i = 0; i < 256; i++) gammaLut[i] = (uint16_t)lroundf(powf(i / 255.0f, 2.2f) * maxDuty);
    for (int i = 0; i < 4; i++) { ledcSetup(i, kPwmHz, kPwmBits); ledcAttachPin(kPins[i], i); ledcWrite(i, 0); }

    Serial.begin(115200);
    delay(100);
    prefs.begin("parkway", false);
    swampDim = prefs.getUChar("swamp_dim", SWAMP_DIM_DEFAULT);
    if (swampDim == 0) swampDim = SWAMP_DIM_DEFAULT;
    setEffect(FX_SWAMP, "boot");
    // One-shot bench show (serial `nextboot`): erased BEFORE use so a crash/reboot can't repeat it.
    int nextBoot = prefs.getInt("nextboot", -1);
    if (nextBoot >= 0) { prefs.remove("nextboot"); showStart((uint32_t)nextBoot * 1000, "one-shot nextboot"); }

    ts(); Serial.printf("[parkway] boot, node 0x%02X, MAC %s, reset reason %d, swamp dim %u\n",
                        (unsigned)THIS_NODE, WiFi.macAddress().c_str(), (int)esp_reset_reason(), (unsigned)swampDim);

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
    delay(10);   // ~100 Hz effect frames; PWM itself runs in hardware

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
