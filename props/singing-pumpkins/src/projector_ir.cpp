#include "projector_ir.h"
#include <Arduino.h>
#include <IRsend.h>
#include <time.h>

static const uint16_t IR_TX_PIN = 17;                    // TX2
static const uint32_t HITACHI_POWER = 0xE1A2E817;       // NEC 32-bit, addr 0x4587 cmd 0x17
static const uint32_t PROJECTOR_BOOT_DELAY_MS = 60000;  // Joe 2026-10-02 (was 15 s, the 2025 value): Hitachi standby + margin
static const uint32_t BOOT_SYNC_WAIT_MS = 60000;        // max extra wait for NTP before the power-up press

// Nightly off (Joe 2026-10-01). Minutes after midnight, index = tm_wday (0 = Sunday).
// "Weekend" = Fri + Sat nights (no school next day).
static const int OFF_MIN[7] = {
    20 * 60 + 30,   // Sun 8:30 PM
    20 * 60 + 30,   // Mon
    20 * 60 + 30,   // Tue
    20 * 60 + 30,   // Wed
    20 * 60 + 30,   // Thu
    21 * 60 + 30,   // Fri 9:30 PM (Joe 2026-10-20: match the show's weekend off; was 9:00)
    21 * 60 + 30,   // Sat 9:30 PM
};
// Halloween night (Joe 2026-10-01: Oct 30 + 31 at 11:00 PM; 2026-10-20: Oct 30 is a normal Friday now).
static const int HALLOWEEN_OFF_MIN = 23 * 60;

static int offMin(const struct tm& t) {
    if (t.tm_mon == 9 && t.tm_mday == 31) return HALLOWEEN_OFF_MIN;
    return OFF_MIN[t.tm_wday];
}

static const int NIGHT_END_MIN = 5 * 60;                // night-off window ends 5:00 AM
static const uint8_t  OFF_PRESSES = 2;                  // press + confirm press (Hitachi)
static const uint32_t OFF_PRESS_GAP_MS = 1500;

static IRsend   s_ir(IR_TX_PIN);
static bool     s_bootPending = false;
static uint32_t s_bootAt = 0;
static int      s_lastOffYday = -1;                    // day-of-year the nightly off already ran
static uint8_t  s_offLeft = 0;
static uint32_t s_offNextAt = 0;

static void ts() { Serial.printf("[%lu] ", (unsigned long)millis()); }

// Last IR action for Home Assistant (2026-10-20): HA cuts the stake extension cord only after it has seen
// "OFF sent <today>" here. IR can't confirm the projector heard it; this at least proves it was SENT.
static char     s_last[80] = "none since boot";
static uint32_t s_lastVer = 0;
static void note(const char* what) {
    struct tm t;
    time_t now = time(nullptr);
    if (now >= 1700000000) {
        localtime_r(&now, &t);
        snprintf(s_last, sizeof s_last, "%s %04d-%02d-%02d %02d:%02d", what, t.tm_year + 1900, t.tm_mon + 1,
                 t.tm_mday, t.tm_hour, t.tm_min);
    } else {
        snprintf(s_last, sizeof s_last, "%s (clock not synced)", what);
    }
    s_lastVer++;
}
const char* projector_ir_last() { return s_last; }
uint32_t projector_ir_version() { return s_lastVer; }

static void sendPower() {
    s_ir.sendNEC(HITACHI_POWER, 32);
    ts(); Serial.printf("[ir] sent projector POWER NEC 0x%08lX\n", (unsigned long)HITACHI_POWER);
}

static bool localNow(struct tm& t) {
    time_t now = time(nullptr);
    if (now < 1700000000) return false;                 // not synced yet
    localtime_r(&now, &t);
    return true;
}

// Inside [today's off time .. 5 AM tomorrow)?
static bool inNightOff(const struct tm& t) {
    int m = t.tm_hour * 60 + t.tm_min;
    if (m >= offMin(t)) return true;
    return m < NIGHT_END_MIN;
}

void projector_ir_begin() {
    s_ir.begin();
    configTzTime("PST8PDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.google.com");
    s_bootPending = true;
    s_bootAt = millis() + PROJECTOR_BOOT_DELAY_MS;
    ts(); Serial.printf("[ir] projector POWER (NEC 0x%08lX) on GPIO%u in %lu s; nightly off "
                        "8:30 PM Sun-Thu, 9:30 PM Fri/Sat, 11:00 PM Oct 31\n",
                        (unsigned long)HITACHI_POWER, IR_TX_PIN, (unsigned long)(PROJECTOR_BOOT_DELAY_MS / 1000));
}

void projector_ir_power() {             // manual (console / HA button): one press
    sendPower();
    note("manual POWER press");
}

void projector_ir_tick() {
    struct tm t;
    bool synced = localNow(t);

    // Power-up press: wait for the clock (up to BOOT_SYNC_WAIT_MS) so a night reboot stays dark.
    if (s_bootPending && (int32_t)(millis() - s_bootAt) >= 0) {
        if (synced) {
            s_bootPending = false;
            if (inNightOff(t)) {
                ts(); Serial.printf("[ir] power-up press SKIPPED: %02d:%02d is in the night-off window\n",
                                    t.tm_hour, t.tm_min);
                s_lastOffYday = t.tm_hour * 60 + t.tm_min >= offMin(t) ? t.tm_yday : -1;
                note("ON skipped (night-off window)");
            } else {
                ts(); Serial.printf("[ir] power-up press (%02d:%02d)\n", t.tm_hour, t.tm_min);
                sendPower();
                note("ON sent (power-up)");
            }
        } else if ((int32_t)(millis() - s_bootAt) >= (int32_t)BOOT_SYNC_WAIT_MS) {
            s_bootPending = false;
            ts(); Serial.println(F("[ir] !! clock not synced after 75 s: power-up press sent anyway"));
            sendPower();
            note("ON sent (power-up) - CLOCK NOT SYNCED: no nightly OFF until it syncs");
        }
    }

    // Nightly off: once per day, at or after the off time (catches up if the loop was busy).
    if (synced && s_offLeft == 0 && t.tm_yday != s_lastOffYday &&
        t.tm_hour * 60 + t.tm_min >= offMin(t)) {
        s_lastOffYday = t.tm_yday;
        s_offLeft = OFF_PRESSES;
        s_offNextAt = millis();
        ts(); Serial.printf("[ir] nightly OFF at %02d:%02d (%u presses)\n", t.tm_hour, t.tm_min, OFF_PRESSES);
        note(OFF_PRESSES == 2 ? "OFF sent (2 presses)" : "OFF sent (1 press)");
    }
    if (s_offLeft && (int32_t)(millis() - s_offNextAt) >= 0) {
        sendPower();
        s_offLeft--;
        s_offNextAt = millis() + OFF_PRESS_GAP_MS;
    }
}
