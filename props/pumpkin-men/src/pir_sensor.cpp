#include "pir_sensor.h"
#include "BlackBox.h"

static bool     s_lastRaw     = false;
static uint32_t s_highSinceMs = 0;
static bool     s_blanked     = false;           // this pulse began in the switching blank
static uint32_t s_pulseAt[PIR_PULSES_NEEDED];    // start times of the last valid pulses
static uint8_t  s_pulseCount  = 0;

void pirInit() {
    pinMode(PIR_PIN, INPUT);
}

bool pirCheck(uint32_t lastSwitchMs) {
    const bool     raw = (digitalRead(PIR_PIN) == HIGH);
    const uint32_t now = millis();
    bool triggered = false;

    if (raw && !s_lastRaw) {                                       // rising edge
        s_highSinceMs = now;
        s_blanked     = lastSwitchMs && now - lastSwitchMs < PIR_BLANK_MS;
    }
    if (!raw && s_lastRaw) {                                       // falling edge: judge the pulse
        const uint32_t len = now - s_highSinceMs;
        if (s_blanked || len < PIR_PULSE_MIN_MS) {
            // Relay/jaw switching puts dozens of 10-30 ms spikes on the line
            // (bench 2026-09-26): count them, one summary line per 30 s.
            static uint32_t s_ign = 0, s_ignLogMs = 0;
            s_ign++;
            if (now - s_ignLogMs >= 30000) {
                s_ignLogMs = now;
                Log.printf("[pir] %lu pulse(s) ignored (switching blank / < %ums glitch), last %lums\n",
                              (unsigned long)s_ign, (unsigned)PIR_PULSE_MIN_MS, (unsigned long)len);
                s_ign = 0;
            }
        } else {
            // drop pulses that fell out of the window, then add this one
            uint8_t k = 0;
            for (uint8_t i = 0; i < s_pulseCount; i++)
                if (s_highSinceMs - s_pulseAt[i] <= PIR_WINDOW_MS) s_pulseAt[k++] = s_pulseAt[i];
            s_pulseCount = k;
            if (s_pulseCount == PIR_PULSES_NEEDED) {               // full: slide
                for (uint8_t i = 1; i < s_pulseCount; i++) s_pulseAt[i - 1] = s_pulseAt[i];
                s_pulseCount--;
            }
            s_pulseAt[s_pulseCount++] = s_highSinceMs;
            Log.printf("[pir] pulse %lums — %u/%u within %lus\n", (unsigned long)len,
                          (unsigned)s_pulseCount, (unsigned)PIR_PULSES_NEEDED, (unsigned long)(PIR_WINDOW_MS / 1000));
            if (s_pulseCount >= PIR_PULSES_NEEDED) {
                triggered    = true;
                s_pulseCount = 0;                                  // next detection needs a fresh train
            }
        }
    }
    s_lastRaw = raw;
    return triggered;
}
