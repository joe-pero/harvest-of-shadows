// First follower prop.
//
// Listens to the show's cue network and "performs" each theme as an LED pattern on the board's
// built-in LED, for as long as a theme track would play. Swap ledShow() for your own prop:
// a relay, a servo, an MP3 module, a strip of lights.
//
// The decisions come from ShowFollower.h, the same tested logic every prop in the real show
// runs. It already handles the awkward cases, so your prop doesn't have to:
//   * only the Scarecrow drives the show; anything else is ignored and counted
//   * each cue arrives 3 times (radio is lossy); repeats are ignored
//   * the theme is whatever the Scarecrow said for THIS visit, never a guess
//   * if the leader goes quiet mid-show, the prop still finishes and returns to idle
//
// Serial monitor (115200) prints every decision with its reason.
#include <Arduino.h>
#include "espnow_link.h"
#include "ShowFollower.h"

static_assert(showfollow::W_CMD_SHOW_STATE == CMD_SHOW_STATE, "ShowFollower wire values drifted");
static_assert(showfollow::W_NODE_SCARECROW == NODE_SCARECROW_A1S, "ShowFollower wire values drifted");

#ifndef LED_PIN
#define LED_PIN 2                      // built-in LED on most ESP32 dev boards
#endif
static const uint32_t TRACK_MS = 30000;   // how long one theme "plays" in this demo (real tracks: ~3 min)

static const char* THEME_NAME[] = {"Children of the Corn", "Return of the Living Dead", "Mutant Pumpkins"};

static showfollow::Follower follower([] {
    showfollow::Config c;
    for (auto& t : c.expectedTrackMs) t = TRACK_MS;
    return c;
}());

static bool playing = false;
static uint8_t playingTheme = 0;
static uint32_t playStart = 0;

static void report(const showfollow::Out& o) {
    if (o.why) Serial.printf("[%7.1fs] %s%s\n", millis() / 1000.0, o.warn ? "WARN " : "", o.why);
}

static void apply(const showfollow::Out& o) {
    report(o);
    if (o.act == showfollow::Act::PLAY) {
        playing = true;
        playingTheme = o.theme;
        playStart = millis();
        Serial.printf("            -> SHOW: %s pattern for %lu s\n", THEME_NAME[o.theme % 3],
                      (unsigned long)(TRACK_MS / 1000));
    } else if (o.act == showfollow::Act::TO_IDLE) {
        playing = false;
        Serial.println("            -> back to idle");
    }
}

// One LED pattern per theme while playing; a slow "breath" while idle.
static void ledShow(uint32_t now) {
    uint32_t t = now - playStart;
    bool on;
    if (!playing) {
        on = (now % 3000) < 150;                                   // idle: a short blip every 3 s
    } else if (playingTheme == THEME_CORN) {
        on = (t % 2000) < 1000;                                    // slow, steady
    } else if (playingTheme == THEME_ROTLD) {
        on = (t % 200) < 100;                                      // frantic
    } else {
        uint32_t p = t % 1200;                                     // pumpkins: double blink
        on = p < 120 || (p > 240 && p < 360);
    }
    digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void setup() {
    Serial.begin(115200);
    delay(300);
    pinMode(LED_PIN, OUTPUT);
    Serial.println("\nFirst follower prop: waiting for the Scarecrow's cues");
    while (!espnow_link::begin()) delay(5000);                     // retry, never give up silently
}

void loop() {
    uint32_t now = millis();
    Msg m;
    while (espnow_link::next(m)) {
        apply(follower.onFrame(m.command, m.sender, m.session_id, m.payload_a, now));
    }
    if (playing && now - playStart >= TRACK_MS) {
        apply(follower.onTrackEnd(now));                           // our "track" finished
    }
    static uint32_t lastTick = 0;
    if (now - lastTick >= 100) {
        lastTick = now;
        apply(follower.tick(now));                                 // fail-safes and timeouts
    }
    static uint32_t lastStatus = 0;
    if (now - lastStatus >= 30000) {
        lastStatus = now;
        const auto& s = follower.stats();
        Serial.printf("[status] plays=%u repeats_ignored=%u other_senders=%u bad_frames=%lu leader=%s\n",
                      s.plays, s.dupIgnored, s.badSender, (unsigned long)espnow_link::rxBad,
                      follower.leaderSilent(now) ? "SILENT" : "ok");
    }
    ledShow(now);
}
