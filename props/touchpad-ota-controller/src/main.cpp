// =============================================================================
// Touchpad "god-mode" override panel  ·  NODE_OTA_CONTROLLER (0x0B)
// =============================================================================
// docs/zombie-container.md
//
// ⚠ FRAMEWORK — not final firmware, but this IS the deployed zombie-container
//   relay controller as of 2026-09-21. Present:
//   - WiFi ch11 + ESP-NOW listener + broadcast origination
//   - momentary relay-pulse to the Zombie Container PicoDMX trigger (1 s, confirmed)
//   - RGB-LED DMX-state indicator (green "water" idle / red / strobe) with the
//     PicoDMX-owns-timing vs ESP32-owns-timing question left flexible
//   - SHOW_ACTIVE_MS = 210 s (shared constant, corrected from 180 s)
//   - CMD_OTA_MODE broadcast + a bench/ops-only `estop` (serial, not touch UI)
//   - Touch UI (hud.cpp), redesigned 2026-09-21: tap ANYWHERE on the
//     containment monitor opens a single full-screen "TAP TO FIRE" override
//     screen; tap ANYWHERE on that fires the zombie (manual fallback for the
//     ESP-NOW auto-fire). No split zones, no ARM/DISARM toggle, no Singing
//     Pumpkins jukebox on touch anymore (arm/disarm stay serial-only). Auto-
//     returns to CONTAINED after 30 s idle with no fire. While a show is
//     running the override screen can't be opened at all — touch is fully
//     locked out and the panel only ever shows the live phase-reactive HUD,
//     in sync with the running show. No on-screen e-stop — nothing here can
//     interrupt a running PicoDMX show once triggered.
// Deferred: the "media/WiZ player" control (device UNCONFIRMED — ASK JOE).
//
// Serial "god-mode" commands (type + Enter):
//   arm / disarm      arm-state for the auto zombie fire
//   fire              manual zombie fire now (relay pulse + DMX segment + broadcast)
//   estop             release all relays, end the DMX segment, broadcast SEGMENT_END
//   ota [min]         broadcast CMD_OTA_MODE + open THIS node's OTA window
//                     (WiFi assoc + ArduinoOTA "halloween-touchpad-ota"; default 15 min)
//   local / external  DMX timing mode (see dmx_state.h)
//   theme 0|1|2       set latched theme (test the idle LED tint hook)
//   state <x>         freeze one HUD/LED state: contained|warning|glass|broken|douse|incinerate|reset
//   demo              toggle auto-cycle of all 5 states
//   idle              toggle the dim wandering STANDBY screensaver (burn-in preview)
//   status            print state
//   trot <0-7>        bench: live-adjust touch offset_rotation (find the value
//                     that matches this panel's physical touch orientation)
//   freeze            bench: freeze the real OVERRIDE screen — taps log, never fire
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include "ProjectConstants.h"
#include "wifi_credentials.h"
#include "pins.h"
#include "espnow_rx.h"
#include "relay_trigger.h"
#include "dmx_state.h"
#include "media_player.h"
#include "segment_protocol.h"
#include "hud.h"
#include "ota_service.h"

// Confirmed 2026-09-19 (Joe) — 1 s simulated PicoDMX button push. Relay chain is
// wired: Hosyond P4 header → JQC-3FF-S-Z 5V relay module (IN = GPIO32) → dry
// contact (NO+COM) → PicoDMX trigger input. Single relay, one trigger.
static const uint32_t PICODMX_PULSE_MS = 1000;

static void ts() { Serial.printf("[%02u:%02u.%03u] ",
    (unsigned)(millis()/60000), (unsigned)((millis()%60000)/1000), (unsigned)(millis()%1000)); }

// ── state ───────────────────────────────────────────────────────────────────
// The zombie fire is part of NORMAL show flow — armed/enabled BY DEFAULT, fires
// automatically when ROTLD is selected (no screen touch needed). "God mode" =
// the panel can DISARM it (suppress) or force a manual FIRE out of sequence.
static bool      g_armed      = true;
static ShowTheme g_theme      = THEME_CORN;
static ShowState g_showState  = SHOW_IDLE;
static uint32_t  g_firedSession = 0;               // dedupe the auto-fire per cycle
static bool      g_demo       = false;             // `demo` cmd toggles a 5-state auto-cycle for bench review

// ── the zombie fire ─────────────────────────────────────────────────────────
static void fireZombie(const char* why) {
    if (ota_service_active()) { ts(); Serial.println("[zombie] FIRE ignored — OTA window open"); return; }
    // Lockout (2026-09-19, Joe): can't re-trigger while a run is already going.
    // dmx_segment_running() auto-clears at DMX_T_END_MS (183920 ms = 3:03.92,
    // the MEASURED PicoDMX show length, dmx_timeline.h) — this is the real show
    // clock, not a placeholder. Covers every caller (serial "fire", touch UI,
    // and the auto-fire path, which already double-checks this itself too).
    if (dmx_segment_running()) { ts(); Serial.println("[zombie] FIRE ignored — show already running"); return; }
    ts(); Serial.printf("[zombie] FIRE (%s)\n", why);
    relay_pulse(RELAY_DMX_PIN, PICODMX_PULSE_MS);   // simulated button push into the PicoDMX
    dmx_segment_start();                            // start LED phase tracking
    espnowBroadcastCmd(CMD_SEGMENT_START, SEGMENT_ZOMBIE, 0, espnowLastSession(), 3);
    g_firedSession = espnowLastSession();
}

static void endZombie(const char* why) {
    ts(); Serial.printf("[zombie] END (%s)\n", why);
    dmx_segment_end();
    espnowBroadcastCmd(CMD_SEGMENT_END, SEGMENT_ZOMBIE, 0, espnowLastSession(), 3);
}

// ── touch UI callback (2026-09-21) — the single override button drives this ─
static void uiFireZombie() { fireZombie("touch UI"); }
// No arm/disarm and no jukebox select on the touch UI anymore (2026-09-21,
// Joe — dropped for the one-button redesign); arm/disarm remain plain serial
// commands below. No uiEstop() (2026-09-19) — removed from the touch UI, see
// hud.h. `estop` stays available as a direct serial command below.

// ── ESP-NOW inbound ─────────────────────────────────────────────────────────
static void onCmd(const Msg& m) {
    switch ((Command)m.command) {
        case CMD_SET_THEME:
            g_theme = (ShowTheme)m.payload_a;
            ts(); Serial.printf("[net] SET_THEME -> %d\n", g_theme);
            // Auto-fire — normal show flow, armed by default. Theme-lock IS the
            // trigger: the ~184 s DMX scene runs concurrently with the Scarecrow
            // monologue on its own clock, not gated on narrator timing.
            if (g_armed && g_theme == THEME_ROTLD
                && m.session_id != g_firedSession && !dmx_segment_running()) {
                fireZombie("armed + ROTLD theme-lock");
            }
            break;

        case CMD_START_SCENE:
            g_theme = (ShowTheme)m.payload_a;
            break;

        case CMD_OTA_MODE:
            // fleet OTA window — this node joins it too (WiFi + ArduinoOTA server)
            ts(); Serial.printf("[net] OTA_MODE -> %lu min\n", (unsigned long)m.payload_a);
            ota_service_start(m.payload_a);
            break;

        case CMD_SHOW_STATE: {
            g_showState = (ShowState)m.payload_a;
            if (g_showState == SHOW_IDLE || g_showState == SHOW_OFF) {
                if (dmx_segment_running()) endZombie("show reset");
            }
            break;
        }

        default: break;
    }

    // An echo of SEGMENT_END while we're running (e.g. from Eave / another panel)
    if (m.command == CMD_SEGMENT_END && m.payload_a == SEGMENT_ZOMBIE && dmx_segment_running()) {
        dmx_segment_end();
    }
    // NOTE: per-phase CMD_SEGMENT_START re-broadcasts (payload_b = SegmentPhaseHint)
    // are not wired to override the LED — the measured timeline (dmx_timeline.h) is
    // authoritative. Add a resync hook here if drift is ever observed.
}

// ── serial ──────────────────────────────────────────────────────────────────
static void printStatus() {
    ts(); Serial.printf("[status] armed=%d theme=%d showState=%d  dmx_running=%d phase=%d timing=%s  relay_busy=%d\n",
                        g_armed, g_theme, g_showState, dmx_segment_running(), dmx_phase(),
                        dmx_timing_mode() == DMX_TIMING_LOCAL ? "LOCAL" : "EXTERNAL", relay_busy());
    ts(); Serial.printf("[status] SHOW_ACTIVE_MS=%lu  dmx_elapsed=%lu ms  strobe_window=%d\n",
                        (unsigned long)SHOW_ACTIVE_MS, (unsigned long)dmx_elapsed_ms(), dmx_strobe_window());
}

static void handleSerial() {
    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c != '\n' && c != '\r') { line += c; continue; }
        line.trim();
        if (line.length()) {
            if      (line == "arm")      { g_armed = true;  ts(); Serial.println("[cmd] ARMED"); }
            else if (line == "disarm")   { g_armed = false; ts(); Serial.println("[cmd] DISARMED"); }
            else if (line == "fire")     fireZombie("manual");
            else if (line.startsWith("fire ")) {
                // bench: fire with the DMX clock pre-advanced — "fire 75" jumps
                // straight into the RED phase, "fire 82" into the strobe overlay.
                uint32_t off = (uint32_t)line.substring(5).toInt() * 1000;
                if (!dmx_segment_running()) {
                    relay_pulse(RELAY_DMX_PIN, PICODMX_PULSE_MS);
                    dmx_segment_start_at(off);
                    espnowBroadcastCmd(CMD_SEGMENT_START, SEGMENT_ZOMBIE, 0, espnowLastSession(), 3);
                    g_firedSession = espnowLastSession();
                }
            }
            else if (line == "estop")    { relay_release_all(); endZombie("e-stop"); }   // bench/ops only — does NOT stop a running PicoDMX show, see hud.h
            else if (line.startsWith("ota")) {
                int sp = line.indexOf(' ');
                uint32_t min = (sp > 0) ? (uint32_t)line.substring(sp + 1).toInt() : 15;
                espnowBroadcastCmd(CMD_OTA_MODE, min, 0, espnowLastSession(), 3);
                ota_service_start(min);          // this node enters the window too
            }
            else if (line == "local")    dmx_set_timing_mode(DMX_TIMING_LOCAL);
            else if (line == "external") dmx_set_timing_mode(DMX_TIMING_EXTERNAL);
            else if (line.startsWith("theme")) {
                // Bench helper: simulates a CMD_SET_THEME cue — sets the theme AND
                // runs the auto-fire check, so ROTLD (theme 1) fires the zombie
                // without needing a second board.
                int sp = line.indexOf(' ');
                if (sp > 0) g_theme = (ShowTheme)line.substring(sp + 1).toInt();
                ts(); Serial.printf("[cmd] theme=%d (simulated SET_THEME cue)\n", g_theme);
                if (g_armed && g_theme == THEME_ROTLD && !dmx_segment_running())
                    fireZombie("bench: armed + ROTLD theme-lock");
            }
            else if (line == "demo")     { g_demo = !g_demo; if (!g_demo) dmx_force_state(-1);
                                           ts(); Serial.printf("[cmd] demo auto-cycle = %d\n", g_demo); }
            else if (line == "idle")     { static bool i = false; i = !i; hud_force_idle(i);
                                           ts(); Serial.printf("[cmd] HUD force-STANDBY = %d\n", i); }
            else if (line.startsWith("state")) {
                g_demo = false;
                String a = line.substring(5); a.trim();
                if      (a == "contained" || a == "green")   dmx_force_state(HS_CONTAINED);
                else if (a == "warning"   || a == "red")     dmx_force_state(HS_WARNING);
                else if (a == "glass"     || a == "break")   dmx_force_state(HS_GLASSBREAK);
                else if (a == "broken"    || a == "white")   dmx_force_state(HS_BROKEN);
                else if (a == "douse"     || a == "amber")   dmx_force_state(HS_DOUSE);
                else if (a == "incinerate"|| a == "fire")    dmx_force_state(HS_INCINERATE);
                else if (a == "reset")                       dmx_force_state(HS_RESET);
                else                                         dmx_force_state(-1);
                ts(); Serial.printf("[cmd] forced state = %d\n", dmx_forced_state());
            }
            else if (line == "status")   printStatus();
            else if (line.startsWith("trot")) {
                int sp = line.indexOf(' ');
                if (sp > 0) hud_set_touch_rotation((uint8_t)line.substring(sp + 1).toInt());
                else { ts(); Serial.println(F("[cmd] usage: trot <0-7> — live touch offset_rotation, bench-only")); }
            }
            else if (line == "freeze")   { hud_start_freezetest(); ts(); Serial.println(F("[cmd] freeze test — real OVERRIDE screen, taps never fire, [freeze] lines follow")); }
            else { ts(); Serial.printf("[cmd] ? '%s'\n", line.c_str()); }
        }
        line = "";
    }
}

// ── MAC self-check — halts if the firmware is on the wrong board ─────────────
static void verifyNodeMac() {
    char expected[18];
    snprintf(expected, sizeof(expected), "%02X:%02X:%02X:%02X:%02X:%02X",
             MAC_OTA_CONTROLLER[0], MAC_OTA_CONTROLLER[1], MAC_OTA_CONTROLLER[2],
             MAC_OTA_CONTROLLER[3], MAC_OTA_CONTROLLER[4], MAC_OTA_CONTROLLER[5]);
    String actual = WiFi.macAddress();
    if (actual.equalsIgnoreCase(expected)) {
        ts(); Serial.printf("[OK] MAC verified: %s — touchpad / OTA controller\n", expected);
        return;
    }
    ts(); Serial.printf("[FATAL] MAC MISMATCH — wrong firmware on this board!\n");
    ts(); Serial.printf("[FATAL] expected %s  got %s\n", expected, actual.c_str());
    ts(); Serial.println(F("[FATAL] if this is genuinely new hardware, update MAC_OTA_CONTROLLER."));
    while (true) { delay(1000); }   // halt — do not run the god-mode panel on the wrong board
}

// ── setup / loop ────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }
    Serial.println();
    ts(); Serial.printf("[main] %s (NODE_OTA_CONTROLLER 0x%02X) — god-mode panel FRAMEWORK\n",
                        NODE_NAME, (unsigned)THIS_NODE);

    relay_setup();
    dmx_setup();
    media_setup();
    hud_setup();                     // ST7796S HUD (owns the backlight)
    hud_set_zombie_callbacks(uiFireZombie);

    espnowInit();                 // brings up the WiFi radio (MAC available after this)
    verifyNodeMac();
    espnowSetCallback(onCmd);
    espnowSendNodeOnline();

    ts(); Serial.println(F("[main] ready — ARMED. cmds: arm disarm fire estop ota theme state demo idle status"));
    ts(); Serial.println(F("[main] boots on CONTAINED (night default) — tap the screen for the control menu"));
    // g_demo defaults to false (2026-09-19) — the panel is now the deployed
    // controller, not a bench-review loop. `demo` still toggles the 5-state
    // auto-cycle on request.
}

// ── demo auto-cycle (temp) — walks all 7 show states ────────────────────────
static void demoTick() {
    if (!g_demo) return;
    static uint32_t next = 0;
    static int step = -1;
    uint32_t now = millis();
    if (now < next) return;
    step = (step + 1) % 7;
    dmx_force_state(step);
    // CONTAINED 12s, WARNING 8s, GLASS BREAK 3s, BROKEN 8s, DOUSE 6s, INCINERATE 10s, RESET 8s
    static const uint32_t dur[7] = { 12000, 8000, 3000, 8000, 6000, 10000, 8000 };
    next = now + dur[step];
}

void loop() {
    handleSerial();
    ota_service_tick();
    espnowTick();
    espnowHeartbeatTick();
    relay_tick();
    dmx_tick(millis());
    demoTick();
    hud_tick();

    static uint32_t alive = 0;
    if (millis() - alive >= 30000) { alive = millis(); printStatus(); }
    delay(2);
}
