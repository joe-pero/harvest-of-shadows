// DMX probe: streams a full DMX512 universe continuously and lets the bench
// set slots over serial, to map an unknown fixture's channels.
//
// Serial (115200), one command per line:
//   s <slot> <val>     set one slot (1-512) to 0-255
//   z                  all slots 0
//   a <val>            all slots 1-512 = val (is the fixture listening at all?)
//   status             frames sent + non-zero slots
//   swamp [breathMs floor]  smooth swamp water on the fixture at `base` (default 5000 40)
//   heart [bpm floor]  red lub-dub heartbeat (default 70 0)
//   zstrobe [level]    zombie: shuffled white strobe bursts 0.2-4 s, black 0.5-9.5 s (level default 140)
//   base <addr>        fixture start address for effects (default 1)
//   hold <0|1>         STOP DMX, drive TX pin as a plain GPIO low/high (code-free converter test)
//   inv <0|1>          invert TX (same effect on the wire as swapping A/B)
#include <Arduino.h>
#include <esp_dmx.h>
#include <driver/uart.h>

static const dmx_port_t kDmxPort = DMX_NUM_1;
static const int kTxPin = 17;   // -> module RXD
static const int kRxPin = 16;   // <- module TXD (unused)

static uint8_t universe[DMX_PACKET_SIZE];   // [0] = start code 0
static uint32_t framesSent = 0;
static bool dmxStopped = false;

// ── swamp water (fixture channel map, from the start address):
//    +0 master, +1 R, +2 G, +3 B, +4 W, +5 mode (0 = manual), +6 strobe/speed
static int fixtureBase = 1;
static bool swampOn = false;
static uint32_t swampBreathMs = 5000;   // one green breath, dim -> bright -> dim
static uint8_t swampFloor = 40;          // never fully dark: it's water
static uint32_t swampT0 = 0;
static float ripple = 0, rippleTarget = 0;
static uint32_t rippleNextAt = 0;

// ── red heartbeat: lub (strong) + dub (70 %, ~260 ms later), fast attack,
//    exponential decay, dark between beats.
static bool heartOn = false;
static uint16_t heartBpm = 70;
static uint8_t heartFloor = 0;
static uint32_t heartT0 = 0;

static float pulse(float tMs, float startMs, float amp) {
  float d = tMs - startMs;
  if (d < 0) return 0;
  if (d < 45) return amp * (d / 45.0f);            // attack
  return amp * expf(-(d - 45) / 110.0f);           // decay
}

static void heartTick() {
  if (!heartOn) return;
  float beatMs = 60000.0f / heartBpm;
  float t = fmodf((float)(millis() - heartT0), beatMs);
  float e = pulse(t, 0, 1.0f) + pulse(t, 260, 0.7f);
  if (e > 1.0f) e = 1.0f;
  uint8_t* f = &universe[fixtureBase];
  f[0] = 255;
  f[1] = (uint8_t)constrain((int)(heartFloor + (255 - heartFloor) * e), 0, 255);
  f[2] = 0; f[3] = 0; f[4] = 0; f[5] = 0; f[6] = 0;   // red only, manual, NO strobe
}

// ── zombie strobe: bursts of ESP-made white flashes, black between. Burst and
//    gap lengths come from two Fisher-Yates-shuffled decks: every length is
//    used once per deck, no back-to-back repeat across a reshuffle.
static const uint16_t kBurstDeck[] = {200, 500, 800, 1200, 1600, 2000, 2500, 3000, 3500, 4000};
static const uint16_t kGapDeck[]   = {500, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 9500};
// Photosensitive limit (WCAG / Ofcom: <= 3 flashes/s): each flash ~50 ms, next
// flash 350-600 ms after the previous one started (<= 2.9 flashes/s), irregular
// so it reads as sparking electrics. Was 10 Hz (40/60 ms) until 2026-09-30.
static const uint16_t kFlashOnMs = 50, kFlashPeriodMinMs = 350, kFlashPeriodMaxMs = 600;

struct Deck {
  const uint16_t* src; uint8_t n; uint16_t order[12]; uint8_t pos; uint16_t last;
  void shuffle() {
    for (uint8_t i = 0; i < n; i++) order[i] = src[i];
    for (int i = n - 1; i > 0; i--) {                  // Fisher-Yates
      int j = esp_random() % (i + 1);
      uint16_t t = order[i]; order[i] = order[j]; order[j] = t;
    }
    if (order[0] == last && n > 1) { uint16_t t = order[0]; order[0] = order[1]; order[1] = t; }
    pos = 0;
  }
  uint16_t next() { if (pos >= n) shuffle(); last = order[pos++]; return last; }
};
static Deck burstDeck = {kBurstDeck, sizeof(kBurstDeck) / 2, {}, 255, 0};
static Deck gapDeck   = {kGapDeck,   sizeof(kGapDeck) / 2,   {}, 255, 0};

static bool zOn = false, zInBurst = false, zFlashLit = false;
static uint8_t zLevel = 140;                 // master during flashes (not full power)
static uint32_t zPhaseEnd = 0, zFlashNext = 0;

static void zTick() {
  if (!zOn) return;
  uint32_t now = millis();
  uint8_t* f = &universe[fixtureBase];
  f[1] = 0; f[2] = 0; f[3] = 0; f[5] = 0; f[6] = 0;     // white only, manual, NO fixture strobe
  if ((int32_t)(now - zPhaseEnd) >= 0) {
    zInBurst = !zInBurst;
    uint16_t d = zInBurst ? burstDeck.next() : gapDeck.next();
    zPhaseEnd = now + d;
    zFlashLit = false; zFlashNext = now;
    Serial.printf("# zombie %s %u ms\n", zInBurst ? "BURST" : "black", d);
  }
  if (zInBurst && (int32_t)(now - zFlashNext) >= 0) {
    zFlashLit = !zFlashLit;
    zFlashNext = now + (zFlashLit ? kFlashOnMs
                                  : (uint32_t)random(kFlashPeriodMinMs, kFlashPeriodMaxMs + 1) - kFlashOnMs);
  }
  bool lit = zInBurst && zFlashLit;
  f[0] = lit ? zLevel : 0;
  f[4] = lit ? 255 : 0;
}

static void swampTick() {
  if (!swampOn) return;
  uint32_t now = millis();
  // 3 green breaths, then one shorter aqua breath (blue swells in and out).
  uint32_t aquaMs = swampBreathMs * 7 / 10;
  uint32_t cycle = swampBreathMs * 3 + aquaMs;
  uint32_t t = (now - swampT0) % cycle;
  bool aqua = t >= swampBreathMs * 3;
  float ph = aqua ? (float)(t - swampBreathMs * 3) / aquaMs : (float)(t % swampBreathMs) / swampBreathMs;
  float breath = 0.5f - 0.5f * cosf(ph * 2.0f * PI);        // 0 -> 1 -> 0, eased

  // slow random ripple: drift toward a new target every 0.3-1.2 s
  if ((int32_t)(now - rippleNextAt) >= 0) {
    rippleTarget = random(-100, 101) / 1000.0f;             // +-10 %
    rippleNextAt = now + random(300, 1200);
  }
  ripple += (rippleTarget - ripple) * 0.05f;

  float g = swampFloor + (255 - swampFloor) * breath;
  g *= (1.0f + ripple);
  float b = aqua ? 170.0f * breath : 12.0f;                  // a hint of blue always, a swell in aqua
  if (aqua) g = swampFloor + (200 - swampFloor) * (0.6f + 0.4f * breath);
  uint8_t* f = &universe[fixtureBase];
  f[0] = 255;                                                // master
  f[1] = 0;                                                  // red
  f[2] = (uint8_t)constrain((int)g, 0, 255);                 // green
  f[3] = (uint8_t)constrain((int)b, 0, 255);                 // blue
  f[4] = 0;                                                  // white
  f[5] = 0;                                                  // manual mode
  f[6] = 0;                                                  // NO strobe
}

// Loopback check: jumper GPIO17 -> GPIO4. Plain UART2 (Serial2) at DMX line
// settings counts breaks + bytes of our own output, proving the frames leave
// the pin with valid timing, without the module or fixture. (esp_dmx's
// DMX_NUM_2 can't be used: its uart.c context table leaves UART2's dev NULL on
// this core -> crash in dmx_driver_install, 2026-09-30.)
static const int kLoopRxPin = 4;
static volatile uint32_t loopBreaks = 0;
static uint32_t loopBytes = 0, loopFF = 0, loopZero = 0;

static void printNonZero() {
  Serial.print("# non-zero:");
  bool any = false;
  for (int i = 1; i < DMX_PACKET_SIZE; i++)
    if (universe[i]) { Serial.printf(" %d=%u", i, universe[i]); any = true; }
  Serial.println(any ? "" : " (none)");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  dmx_config_t config = DMX_CONFIG_DEFAULT;
  dmx_personality_t personalities[] = {};
  dmx_driver_install(kDmxPort, &config, personalities, 0);
  dmx_set_pin(kDmxPort, kTxPin, kRxPin, DMX_PIN_NO_CHANGE);  // auto-direction module: no RTS
  memset(universe, 0, sizeof(universe));
  Serial2.setRxBufferSize(2048);
  Serial2.begin(250000, SERIAL_8N2, kLoopRxPin, -1);
  Serial2.onReceiveError([](hardwareSerial_error_t e) { if (e == UART_BREAK_ERROR) loopBreaks++; });
  Serial.printf("\n# dmx probe ready: TX GPIO%d. cmds: s <slot> <val> | z | status\n", kTxPin);
}

void loop() {
  // Continuous stream: fixtures expect a steady refresh (~44 Hz full universe).
  if (dmxStopped) { delay(20); goto serial_in; }
  swampTick();
  heartTick();
  zTick();
  dmx_write(kDmxPort, universe, DMX_PACKET_SIZE);
  dmx_send(kDmxPort);
  dmx_wait_sent(kDmxPort, DMX_TIMEOUT_TICK);
  framesSent++;

  while (Serial2.available()) {
    int c = Serial2.read();
    loopBytes++;
    if (c == 0xFF) loopFF++; else if (c == 0x00) loopZero++;
  }

serial_in:
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.startsWith("s ")) {
    int sp = line.indexOf(' ', 2);
    int slot = line.substring(2, sp).toInt();
    int val = line.substring(sp + 1).toInt();
    if (slot >= 1 && slot < DMX_PACKET_SIZE && val >= 0 && val <= 255) {
      universe[slot] = val;
      Serial.printf("# slot %d = %d\n", slot, val);
    } else Serial.println("# bad s args");
  } else if (line.startsWith("a ")) {
    swampOn = false; heartOn = false; zOn = false;
    int val = constrain(line.substring(2).toInt(), 0, 255);
    memset(universe + 1, val, DMX_PACKET_SIZE - 1);
    Serial.printf("# all slots = %d\n", val);
  } else if (line.startsWith("swamp")) {
    int a = 0, b2 = 0;
    if (sscanf(line.c_str(), "swamp %d %d", &a, &b2) >= 1 && a >= 1000) swampBreathMs = a;
    if (b2 > 0 && b2 < 255) swampFloor = b2;
    memset(universe + 1, 0, DMX_PACKET_SIZE - 1);
    heartOn = false; zOn = false; swampOn = true; swampT0 = millis();
    Serial.printf("# swamp ON at base %d, breath %lu ms, floor %u\n", fixtureBase, (unsigned long)swampBreathMs, swampFloor);
  } else if (line.startsWith("heart")) {
    int a = 0, b2 = -1;
    if (sscanf(line.c_str(), "heart %d %d", &a, &b2) >= 1 && a >= 30 && a <= 180) heartBpm = a;
    if (b2 >= 0 && b2 < 200) heartFloor = b2;
    memset(universe + 1, 0, DMX_PACKET_SIZE - 1);
    swampOn = false; zOn = false; heartOn = true; heartT0 = millis();
    Serial.printf("# heartbeat ON at base %d, %u bpm, floor %u\n", fixtureBase, heartBpm, heartFloor);
  } else if (line.startsWith("zstrobe")) {
    int a = 0;
    if (sscanf(line.c_str(), "zstrobe %d", &a) == 1 && a > 0 && a <= 255) zLevel = a;
    memset(universe + 1, 0, DMX_PACKET_SIZE - 1);
    swampOn = false; heartOn = false; zOn = true; zInBurst = false;
    zPhaseEnd = millis() + gapDeck.next();              // start in black
    Serial.printf("# zombie strobe ON at base %d, level %u\n", fixtureBase, zLevel);
  } else if (line.startsWith("base ")) {
    fixtureBase = constrain(line.substring(5).toInt(), 1, 506);
    Serial.printf("# fixture base %d\n", fixtureBase);
  } else if (line.startsWith("hold ")) {
    int lvl = line.substring(5).toInt() ? HIGH : LOW;
    dmxStopped = true;
    pinMatrixOutDetach(kTxPin, false, false);   // take the pin back from the UART
    pinMode(kTxPin, OUTPUT);
    digitalWrite(kTxPin, lvl);
    Serial.printf("# DMX STOPPED, GPIO%d held %s (reset board to resume DMX)\n", kTxPin, lvl ? "HIGH 3.3V" : "LOW 0V");
  } else if (line.startsWith("inv ")) {
    bool on = line.substring(4).toInt() != 0;
    esp_err_t e = uart_set_line_inverse((uart_port_t)kDmxPort,
                                        on ? UART_SIGNAL_TXD_INV : UART_SIGNAL_INV_DISABLE);
    Serial.printf("# TX invert %s (%s)\n", on ? "ON" : "OFF", esp_err_to_name(e));
  } else if (line == "z") {
    swampOn = false; heartOn = false; zOn = false;
    memset(universe, 0, sizeof(universe));
    Serial.println("# all zero");
  } else if (line == "status") {
    Serial.printf("# frames sent %lu\n", (unsigned long)framesSent);
    Serial.printf("# loopback (GPIO%d): breaks %lu bytes %lu (0xFF %lu, 0x00 %lu)\n",
                  kLoopRxPin, (unsigned long)loopBreaks, (unsigned long)loopBytes,
                  (unsigned long)loopFF, (unsigned long)loopZero);
    printNonZero();
  } else if (line.length()) Serial.printf("# ? '%s'\n", line.c_str());
}
