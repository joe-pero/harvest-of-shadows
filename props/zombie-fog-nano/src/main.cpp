// Zombie Container fog relay — standalone Arduino Nano
//
// Starts on power-up and loops forever: fog burst, pause, fog burst, pause...
// Burst lengths and pauses are each dealt from a Fisher-Yates-shuffled deck,
// reshuffled when empty, so every value gets used once per deck and the
// bursts stay evenly spread (no clumps, no long dead stretches).
//
// Average: 5.5 s burst + 11.5 s pause = one burst per ~17 s.
// Pause is measured from the end of one burst to the start of the next, so
// bursts are always at least GAPS_MS[min] apart.
//
// Fog relay on D3, board jumper set to HIGH trigger.
// Light relay on D4 (red light on the fog machine): on from power-up, stays on.

#include <Arduino.h>

const uint8_t  RELAY_PIN        = 3;
const uint8_t  LIGHT_PIN        = 4;      // red light, on the whole time
const bool     RELAY_ACTIVE_LOW = false;  // board jumper set to HIGH trigger

const uint16_t BURSTS_MS[] = {3000, 4000, 5000, 6000, 7000, 8000};  // 8 s max on
const uint16_t GAPS_MS[]   = {9000, 10000, 11000, 12000, 13000, 14000};

const uint8_t N_BURSTS = sizeof(BURSTS_MS) / sizeof(BURSTS_MS[0]);
const uint8_t N_GAPS   = sizeof(GAPS_MS) / sizeof(GAPS_MS[0]);

// A deck of indices into a value table; deal() reshuffles when empty.
struct Deck {
  uint8_t order[8];
  uint8_t n;
  uint8_t next;

  void init(uint8_t count) { n = count; next = n; }

  uint8_t deal() {
    if (next >= n) {
      for (uint8_t i = 0; i < n; i++) order[i] = i;
      for (uint8_t i = n - 1; i > 0; i--) {   // Fisher-Yates
        uint8_t j = random(i + 1);            // 0..i inclusive
        uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
      }
      next = 0;
    }
    return order[next++];
  }
};

Deck burstDeck, gapDeck;

void setRelay(uint8_t pin, bool on) {
  digitalWrite(pin, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

void relay(bool on) { setRelay(RELAY_PIN, on); }

// Mix several floating-pin reads so each power-up gets a different pattern.
void seedRandom() {
  uint32_t seed = 0;
  for (uint8_t i = 0; i < 32; i++) {
    seed = (seed << 1) ^ analogRead(A0) ^ analogRead(A1) ^ micros();
    delay(1);
  }
  randomSeed(seed);
}

void setup() {
  relay(false);                 // set the level BEFORE enabling the output
  pinMode(RELAY_PIN, OUTPUT);   // so the relay never blips on at boot
  relay(false);

  setRelay(LIGHT_PIN, true);    // red light on for as long as there's power
  pinMode(LIGHT_PIN, OUTPUT);
  setRelay(LIGHT_PIN, true);

  Serial.begin(115200);
  seedRandom();
  burstDeck.init(N_BURSTS);
  gapDeck.init(N_GAPS);
  Serial.println(F("zombie-fog-nano: up"));
}

void loop() {
  uint16_t gap   = GAPS_MS[gapDeck.deal()];
  uint16_t burst = BURSTS_MS[burstDeck.deal()];

  Serial.print(F("wait ")); Serial.print(gap);
  Serial.print(F(" ms  fog ")); Serial.print(burst); Serial.println(F(" ms"));

  delay(gap);
  relay(true);
  delay(burst);
  relay(false);
}
