// Test sender: stands in for the Scarecrow so you can try a follower with two boards and no show.
//
// Every minute it plays one pretend visit, cycling through the three themes, using the same cue
// sequence the real Scarecrow sends:
//
//   pre-show   SET_THEME(theme) + SHOW_STATE(NARRATOR)      each sent 3 times
//   +8 s       START_SCENE(theme) + SHOW_STATE(ACTIVE)      each sent 3 times
//   always     HEARTBEAT every 10 s
//
// TEST USE ONLY: it identifies itself as the Scarecrow, so don't run it near a live show.
#include <Arduino.h>
#include "espnow_link.h"

static const uint32_t VISIT_EVERY_MS = 60000;
static const uint32_t PRESHOW_TO_SHOW_MS = 8000;
static const char* THEME_NAME[] = {"Children of the Corn", "Return of the Living Dead", "Mutant Pumpkins"};

static uint32_t session = 0;
static uint8_t theme = 0;

// The real Scarecrow sends every cue 3 times, a few ms apart; followers ignore the repeats.
static void burst(uint8_t cmd, uint32_t a) {
    for (int i = 0; i < 3; i++) {
        if (!espnow_link::send(cmd, NODE_SCARECROW_A1S, session, a)) Serial.println("  send FAILED");
        delay(15);
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nTest sender (pretending to be the Scarecrow). TEST USE ONLY.");
    while (!espnow_link::begin()) delay(5000);
}

void loop() {
    static uint32_t lastVisit = 0, lastBeat = 0, showAt = 0;
    static bool pendingShow = false;
    uint32_t now = millis();

    if (lastVisit == 0 || now - lastVisit >= VISIT_EVERY_MS) {
        lastVisit = now ? now : 1;
        session = esp_random();
        Serial.printf("[%6.1fs] visit 0x%08lX: pre-show, theme %s\n", now / 1000.0, (unsigned long)session,
                      THEME_NAME[theme]);
        burst(CMD_SET_THEME, theme);
        burst(CMD_SHOW_STATE, SHOW_NARRATOR);
        pendingShow = true;
        showAt = now + PRESHOW_TO_SHOW_MS;
    }
    if (pendingShow && (int32_t)(now - showAt) >= 0) {
        pendingShow = false;
        Serial.printf("[%6.1fs]   main show\n", now / 1000.0);
        burst(CMD_START_SCENE, theme);
        burst(CMD_SHOW_STATE, SHOW_ACTIVE);
        theme = (uint8_t)((theme + 1) % 3);
    }
    if (now - lastBeat >= 10000) {
        lastBeat = now;
        espnow_link::send(CMD_HEARTBEAT, NODE_SCARECROW_A1S, session, 0);
    }
    delay(5);
}
