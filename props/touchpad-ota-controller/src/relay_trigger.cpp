#include "relay_trigger.h"
#include "pins.h"

#define MAX_RELAYS 4
struct Pulse { uint8_t pin; uint32_t endMs; bool active; };
static Pulse s_pulses[MAX_RELAYS];

static const uint8_t RELAY_PINS[] = { RELAY_DMX_PIN, RELAY_AUX_PIN };

static void deassert(uint8_t pin) {
    digitalWrite(pin, RELAY_ACTIVE_LEVEL == LOW ? HIGH : LOW);
}
static void assert_(uint8_t pin) {
    digitalWrite(pin, RELAY_ACTIVE_LEVEL);
}

void relay_setup() {
    for (uint8_t p : RELAY_PINS) {
        pinMode(p, OUTPUT);
        deassert(p);
    }
    for (auto& x : s_pulses) x.active = false;
}

void relay_pulse(uint8_t pin, uint32_t ms) {
    // reuse an existing slot for this pin, else a free slot
    Pulse* slot = nullptr;
    for (auto& x : s_pulses) if (x.active && x.pin == pin) { slot = &x; break; }
    if (!slot) for (auto& x : s_pulses) if (!x.active) { slot = &x; break; }
    if (!slot) { Serial.println(F("[relay] no free pulse slot")); return; }

    slot->pin    = pin;
    slot->endMs  = millis() + ms;
    slot->active = true;
    assert_(pin);
    Serial.printf("[relay] pulse GPIO%u for %lu ms\n", pin, (unsigned long)ms);
}

void relay_tick() {
    uint32_t now = millis();
    for (auto& x : s_pulses) {
        if (x.active && (int32_t)(now - x.endMs) >= 0) {
            deassert(x.pin);
            x.active = false;
            Serial.printf("[relay] GPIO%u released\n", x.pin);
        }
    }
}

bool relay_busy() {
    for (auto& x : s_pulses) if (x.active) return true;
    return false;
}

void relay_release_all() {
    for (uint8_t p : RELAY_PINS) deassert(p);
    for (auto& x : s_pulses) x.active = false;
    Serial.println(F("[relay] ALL released (e-stop)"));
}
