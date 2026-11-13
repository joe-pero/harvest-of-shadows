// =============================================================================
// Singing Pumpkins — 2026 node (docs/singing-pumpkins.md)
// =============================================================================
// This ESP32 is the CONTROLLER: it listens to the fleet over ESP-NOW and tells
// the BrightSign XT1143 what phase the show is in, one serial line at a time.
// The playback logic (DJ shuffle, weighting, opening -> clips -> close-out) lives
// on the player's SD card: props/singing-pumpkins/sdcard/autorun.brs.
//
//   CMD_SHOW_STATE -> SHOW_ACTIVE (edge)        : "SHOW"  (card runs the ~3 min trick-or-treat show)
//   CMD_SHOW_STATE -> SHOW_IDLE / SHOW_OFF      : "END"   (card closes out if still running)
//   CMD_JUKEBOX_SELECT (touch panel)            : "PLAY n" / "PLAY n NOW"
//   Home Assistant (MQTT, ha_mqtt.h)            : Play now / Start show / End show / Skip / Volume
//   USB serial line typed at the bench          : forwarded to the card as-is
//
//   Skeletons (skeletons.h)                     : 2 jaw relays (GPIO25/26), rare moments from the
//                                                 card's CUES line (offline audio scan)
//   Projector (projector_ir.h)                  : Hitachi POWER over IR: on 60 s after boot,
//                                                 off 8:30 PM Sun-Thu / 9:00 PM Fri+Sat /
//                                                 11:00 PM Oct 30+31 (NTP)
//
// Mesh role: Tier 1, receive-only.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include "ProjectConstants.h"
#include "wifi_credentials.h"
#include "espnow_rx.h"
#include "brightsign.h"
#include "ha_mqtt.h"
#include "projector_ir.h"
#include "skeletons.h"

static void ts() { Serial.printf("[%02u:%02u.%03u] ",
    (unsigned)(millis()/60000), (unsigned)((millis()%60000)/1000), (unsigned)(millis()%1000)); }

// ── FL-1 mode ───────────────────────────────────────────────────────────────
// MODE_NARRATOR sends nothing; it exists so the next SHOW_ACTIVE is a fresh edge
// even when the leader goes ACTIVE -> NARRATOR -> ACTIVE with no IDLE between visits.
enum PumpkinMode : uint8_t { MODE_IDLE_AMBIENT, MODE_NARRATOR, MODE_ACTIVE_SHOW };
static PumpkinMode g_mode      = MODE_IDLE_AMBIENT;
static PumpkinMode g_wantMode  = MODE_IDLE_AMBIENT;
static ShowTheme   g_theme     = THEME_CORN;
static uint32_t    g_lastShowStateMs = 0;

// If no SHOW_STATE is heard for this long (leader down / off-season) -> idle-ambient (FL-1 fallback)
static const uint32_t SHOWSTATE_STALE_MS = 15UL * 60UL * 1000UL;

// ── PLACEHOLDER (Joe 2026-09-28): narrators own the floor ────────────────────
// "I don't want the pumpkins talking over Burt and the Scarecrow. The Scarecrow
// is the main narrator." Today a new PRE-SHOW arriving mid pumpkin-show leaves
// the card singing right over the narrator. Planned (docs/audio-floor-control.md):
// duck the card (VOL down, or pause) while a narrator speaks, restore after,
// with the per-song vocal map (Whisper + energy) choosing soft/instrumental
// spots. Encores/HOLD taunts don't send any SHOW_STATE yet, so this hook can't
// see them until the Scarecrow broadcasts a "narrator speaking" signal.
// NOTHING IS SENT TO THE CARD HERE YET: log only, no behavior change.
static void narratorFloorPlaceholder() {
    ts(); Serial.println(F("[floor] PLACEHOLDER: narrator starting while the card may be singing "
                           "-- duck/pause not implemented (docs/audio-floor-control.md)"));
}

static void onCmd(const Msg& m) {
    switch ((Command)m.command) {
        case CMD_SET_THEME:
            g_theme = (ShowTheme)m.payload_a;
            ts(); Serial.printf("[main] SET_THEME -> %d\n", g_theme);
            break;

        case CMD_START_SCENE:
            g_theme = (ShowTheme)m.payload_a;
            break;

        case CMD_JUKEBOX_SELECT: {
            uint8_t songId   = (uint8_t)m.payload_a;
            bool    playNext = m.payload_b != 0;
            ts(); Serial.printf("[jukebox] SELECT song=%u mode=%s\n", songId, playNext ? "NEXT" : "NOW");
            // Song number = library.json "n" on the card (see sdcard/TRACKS.md).
            brightsign_jukebox(songId, !playNext);   // touchpad path (secondary; HA is the main override)
            break;
        }

        case CMD_SHOW_STATE: {
            g_lastShowStateMs = millis();
            ShowState st = (ShowState)m.payload_a;
            // The pumpkins' show starts when the narrators are done (SHOW_ACTIVE).
            // SHOW_NARRATOR leaves the card as it is; IDLE/OFF ends the show.
            if (st == SHOW_ACTIVE)                        g_wantMode = MODE_ACTIVE_SHOW;
            else if (st == SHOW_NARRATOR)                 g_wantMode = MODE_NARRATOR;
            else if (st == SHOW_IDLE || st == SHOW_OFF)   g_wantMode = MODE_IDLE_AMBIENT;
            break;
        }

        default: break;
    }
}

// Bench: a line typed on USB serial goes straight to the card (SHOW, END, PLAY 12, STATUS...),
// except SWAP / PINS (TX/RX wiring check), IRPWR (projector POWER) and SKEL ... (skeleton jaws),
// which the ESP32 handles itself.
static void benchConsoleTick() {
    static char buf[48];
    static size_t len = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            if (len) {
                buf[len] = 0;
                if (skel_console(buf))             ;                         // SKEL ... (ESP32-local)
                else if (!strcasecmp(buf, "SWAP"))      brightsign_swap_pins();    // ESP32-local, not sent
                else if (!strcasecmp(buf, "PINS")) brightsign_print_pins();
                else if (!strcasecmp(buf, "IRPWR")) projector_ir_power();
                else                               brightsign_send(buf);
                len = 0;
            }
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

void setup() {
    skel_begin();                  // jaw relays OFF before anything else (pins float until now)
    // Player TX line idle (HIGH) right away: floating until Serial1 starts after WiFi (~1.4 s),
    // the HW-027 turned it into junk the card answered with "ERR unknown command" (2026-10-02).
    pinMode(BS_TX_PIN, OUTPUT);
    digitalWrite(BS_TX_PIN, HIGH);
    Serial.begin(115200);
    while (!Serial) { ; }
    Serial.println();
    ts(); Serial.printf("[main] Singing Pumpkins (NodeID 0x%02X) booting\n",
                        (unsigned)THIS_NODE);

    skel_console("SKEL");          // skeleton settings (skel_begin ran before Serial was up)
    espnowInit();                  // WiFi STA (joins the ch-11 AP) + ESP-NOW RX
    ha_begin();                    // Home Assistant control over MQTT
    espnowSetCallback(onCmd);
    ts(); Serial.printf("[main] MAC %s — record vs MAC_SINGING_PUMPKINS\n", WiFi.macAddress().c_str());

    brightsign_begin();   // the card boots into DJ mode on its own
    projector_ir_begin(); // one POWER press once the projector is in standby
    g_mode = g_wantMode = MODE_IDLE_AMBIENT;

    espnowSendNodeOnline();
    ts(); Serial.println(F("[main] ready — listening for SHOW_STATE / JUKEBOX_SELECT"));
}

void loop() {
    espnowTick();
    espnowHeartbeatTick();
    brightsign_tick();
    ha_tick();
    projector_ir_tick();
    skel_tick();
    benchConsoleTick();

    // FL-1 stale-signal fallback
    if (g_lastShowStateMs != 0 && (millis() - g_lastShowStateMs) > SHOWSTATE_STALE_MS) {
        g_wantMode = MODE_IDLE_AMBIENT;
    }

    if (g_wantMode != g_mode) {
        g_mode = g_wantMode;
        skel_set_narrator(g_mode == MODE_NARRATOR);   // narrators own the floor: no jaw moments
        if (g_mode == MODE_ACTIVE_SHOW) {
            ts(); Serial.println(F("[main] -> ACTIVE-SHOW: card SHOW"));
            brightsign_show_start();
        } else if (g_mode == MODE_NARRATOR) {
            ts(); Serial.println(F("[main] -> NARRATOR (card keeps playing; next ACTIVE starts a show)"));
            narratorFloorPlaceholder();
        } else {
            ts(); Serial.println(F("[main] -> IDLE: card END"));
            brightsign_show_end();
        }
    }

    static uint32_t s_alive = 0;
    if (millis() - s_alive >= 60000) {
        s_alive = millis();
        ts(); Serial.printf("[main] alive  mode=%s theme=%d card=%s link=%s up=%lus\n",
                            g_mode == MODE_ACTIVE_SHOW ? "ACTIVE" : g_mode == MODE_NARRATOR ? "NARRATOR" : "IDLE",
                            g_theme, brightsign_card_mode(),
                            brightsign_link_ok() ? "ok" : "LOST (no heartbeat from the card)",
                            (unsigned long)(millis()/1000));
    }

    delay(10);
}
