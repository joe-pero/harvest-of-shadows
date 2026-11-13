// =============================================================================
// Zombie Container relay — ESP-01S RELAY V4.0 (IOT-MCU)
// =============================================================================
// THIS is now the main trigger for the Zombie Container prop (2026-09-21,
// Joe). Two independent ESP-NOW trigger sources, see onDataRecv() below:
//   1. PRIMARY — Scarecrow's own CMD_SET_THEME=THEME_ROTLD broadcast, heard
//      directly. Self-sufficient: fires even if the touchpad is off/crashed.
//   2. BACKDOOR OVERRIDE — the touchpad's existing CMD_SEGMENT_START /
//      SEGMENT_ZOMBIE broadcast (props/touchpad-ota-controller/src/main.cpp
//      fireZombie(), sent on every touchpad fire — touch UI or serial `fire`
//      — no touchpad firmware change needed), for a manual trigger when the
//      container isn't mid-cycle.
// Drives its own onboard relay straight into the PicoDMX trigger input.
// Replaces the loose-wire GPIO32-to-bare-relay-module wiring on the touchpad
// that ate a whole bench session (see platformio.ini header +
// docs/zombie-container.md + PROJECT_LOG.md 2026-09-21).
//
// GPIO0 / active-LOW confirmed against the IOT-MCU ESP-01S-Relay-v4.0 repo
// and the Tasmota device template for this exact board (not guessed):
//   https://github.com/IOT-MCU/ESP-01S-Relay-v4.0
//   https://templates.blakadder.com/ESP-01S-Relay-v4.html ("Relay_i" = the
//   Tasmota naming convention for an inverted/active-low relay GPIO)
// Known quirk: GPIO0 doubles as the ESP8266 boot-strap pin, so a brief relay
// flicker at power-on/reset is a documented hardware limitation of this
// board (see GitHub issue IOT-MCU/ESP-01S-Relay-v4.0#1), not a bug here.
//
// ESP-NOW init pattern mirrors the proven-working area-lights ESP-01S nodes
// (~/Documents/Halloween-Dev/PlatformIO/area-lights/src/main.cpp) — native
// espnow.h/user_interface.h, channel-locked, no AP association. Wire format
// is the fleet's real 22-byte Msg (ProjectConstants.h), not area-lights' own
// 6-byte CmdPacket.
//
// Serial commands (115200 baud): fire (bench: pulse the relay directly,
// bypassing ESP-NOW entirely), status.
// =============================================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
extern "C" {
#include <espnow.h>
#include <user_interface.h>
}
#include "ProjectConstants.h"
#include "wifi_credentials.h"
#include "segment_protocol.h"

// ── relay ─────────────────────────────────────────────────────────────────
#define RELAY_PIN            0     // GPIO0 — confirmed against the board's own repo, see header
#define RELAY_ACTIVE_LEVEL   LOW   // confirmed active-low, see header
static const uint32_t PICODMX_PULSE_MS = 1000;   // matches the touchpad's own pulse width

static void relayDeassert() { digitalWrite(RELAY_PIN, RELAY_ACTIVE_LEVEL == LOW ? HIGH : LOW); }
static void relayAssert()   { digitalWrite(RELAY_PIN, RELAY_ACTIVE_LEVEL); }

static bool     s_pulseActive = false;
static uint32_t s_pulseEndMs  = 0;

static void relayPulse(uint32_t ms, const char* why) {
    relayAssert();
    s_pulseActive = true;
    s_pulseEndMs  = millis() + ms;
    Serial.printf("[relay] pulse GPIO%u for %lu ms (%s)\n", RELAY_PIN, (unsigned long)ms, why);
}

static void relayTick() {
    if (s_pulseActive && (int32_t)(millis() - s_pulseEndMs) >= 0) {
        relayDeassert();
        s_pulseActive = false;
        Serial.println(F("[relay] released"));
    }
}

// ── ESP-NOW receive — kept minimal, no Serial/blocking calls in the callback ─
// Two independent trigger sources, both gated by the same per-cycle
// session_id dedupe (Scarecrow's theme broadcast and the touchpad's own fire
// broadcast carry the same session_id for a given cycle — the touchpad
// copies it from the last SET_THEME it saw — so one shared dedupe correctly
// suppresses a double-fire if both arrive for the same cycle):
//   1. PRIMARY — Scarecrow's own CMD_SET_THEME=THEME_ROTLD broadcast, heard
//      directly. This is now self-sufficient: the relay fires the DMX show
//      even if the touchpad is powered off, crashed, or mid-touch-testing.
//   2. BACKDOOR OVERRIDE — the touchpad's existing CMD_SEGMENT_START /
//      SEGMENT_ZOMBIE broadcast (sent on every touchpad fire — touch UI,
//      serial `fire`, or its own now-redundant auto-fire path). Lets Joe
//      manually trigger the show from the touchpad when the container isn't
//      mid-cycle, without needing physical access to this relay board.
enum FireReason : uint8_t { FIRE_NONE = 0, FIRE_ROTLD_THEME, FIRE_TOUCHPAD_BACKDOOR };

static volatile uint8_t  s_fireReason  = FIRE_NONE;
static volatile uint32_t s_rxSession   = 0;
static uint32_t          s_lastFiredSession = 0;

static void onDataRecv(uint8_t* /*mac*/, uint8_t* data, uint8_t len) {
    if (len != (uint8_t)sizeof(Msg)) return;
    Msg m;
    memcpy(&m, data, sizeof(Msg));
    if (!msg_valid(m)) return;
    if (m.target != (uint8_t)NODE_BROADCAST && m.target != (uint8_t)THIS_NODE) return;

    if (m.command == (uint8_t)CMD_SET_THEME && m.payload_a == (uint32_t)THEME_ROTLD) {
        s_rxSession  = m.session_id;
        s_fireReason = FIRE_ROTLD_THEME;
    } else if (m.command == (uint8_t)CMD_SEGMENT_START && m.payload_a == (uint32_t)SEGMENT_ZOMBIE) {
        s_rxSession  = m.session_id;
        s_fireReason = FIRE_TOUCHPAD_BACKDOOR;
    }
}

static void handleFireRequest() {
    uint8_t reason = s_fireReason;
    if (reason == FIRE_NONE) return;
    s_fireReason = FIRE_NONE;
    uint32_t session = s_rxSession;
    // Dedupe: both sources broadcast their command 3x (ESP-NOW reliability),
    // and Scarecrow's theme-lock + the touchpad's own fire share the same
    // session_id per cycle — only pulse once per session_id, same pattern
    // the touchpad itself uses for its own auto-fire dedupe.
    if (session != 0 && session == s_lastFiredSession) return;
    s_lastFiredSession = session;
    relayPulse(PICODMX_PULSE_MS, reason == FIRE_ROTLD_THEME
                                  ? "espnow CMD_SET_THEME/THEME_ROTLD (primary)"
                                  : "espnow CMD_SEGMENT_START/SEGMENT_ZOMBIE (touchpad backdoor)");
}

// ── serial ────────────────────────────────────────────────────────────────
static void printStatus() {
    Serial.printf("[status] pulseActive=%d lastFiredSession=%lu MAC=%s\n",
                  s_pulseActive, (unsigned long)s_lastFiredSession, WiFi.macAddress().c_str());
}

static void handleSerial() {
    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c != '\n' && c != '\r') { line += c; continue; }
        line.trim();
        if (line.length()) {
            if      (line == "fire")   relayPulse(PICODMX_PULSE_MS, "serial bench test");
            else if (line == "status") printStatus();
            else                       Serial.printf("[cmd] ? '%s'\n", line.c_str());
        }
        line = "";
    }
}

// ── setup / loop ─────────────────────────────────────────────────────────
void setup() {
    delay(3000);   // power-rail stabilization before WiFi init — matches area-lights' proven pattern
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.printf("[zombie-relay] booting (node 0x%02X)\n", (unsigned)THIS_NODE);

    pinMode(RELAY_PIN, OUTPUT);
    relayDeassert();   // idle OFF as early as possible — minimizes (does not eliminate) the boot-strap flicker

    uint8_t mac[6];
    wifi_get_macaddr(STATION_IF, mac);
    Serial.printf("[zombie-relay] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    wifi_set_channel(WIFI_CHANNEL);
    if (esp_now_init() != 0) {
        Serial.println(F("[espnow] init FAILED — rebooting in 3s"));
        delay(3000);
        ESP.restart();
    }
    esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
    esp_now_register_recv_cb(onDataRecv);
    Serial.printf("[espnow] ready on channel %u\n", WIFI_CHANNEL);

    Serial.println(F("[zombie-relay] ready. cmds: fire status"));
}

void loop() {
    handleSerial();
    handleFireRequest();
    relayTick();

    static uint32_t alive = 0;
    if (millis() - alive >= 30000) { alive = millis(); printStatus(); }
}
