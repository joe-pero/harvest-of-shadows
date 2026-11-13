#include "pir_sensor.h"

static pirlogic::PirEdge s_edge;
static pirlogic::PirGate s_gate;

void pirInit() {
    pinMode(PIR_PIN, INPUT_PULLUP);
    s_gate.cooldownMs = PIR_COOLDOWN_MS;
    // Start from the real line level so a relay already closed at boot is not a trip.
    s_edge.rawLow = s_edge.closed = (digitalRead(PIR_PIN) == LOW);
    s_edge.rawSince = s_edge.closedAt = millis();
}

pirlogic::Ev pirTick(uint32_t now) {
    return s_edge.update(digitalRead(PIR_PIN) == LOW, now);
}

const pirlogic::PirEdge& pirEdge() { return s_edge; }
pirlogic::PirGate& pirGate() { return s_gate; }
