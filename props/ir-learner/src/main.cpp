// IR learner: dumps every received code (protocol, hex, raw timings) and can
// replay the last one so we can prove the capture actually drives the LEDs.
//
// Serial commands (115200):
//   r  replay last captured code (decoded protocol if known, else raw)
//   R  replay last captured code as raw timings
//   n<hex>\n  send a 32-bit NEC code, e.g. "nFF1AE5" (survives resets)
//   w[min,max]\n  start swamp-water effect: GREEN<->AQUA, random dwell in ms
//                 (default 1500,5000), e.g. "w800,3000"
//   f[steps,stepMs,greenCycles]\n  start swamp FADE: GREEN breathes low->high->low
//                 greenCycles times, then one quick AQUA breath, repeat.
//                 Default 8,250,3. Color swaps happen at the dim floor.
//   a<hex>\n  color for the fade's accent breath (default AQUA 0xFF8A75)
//   S  preset: swamp water (fast green breathing + touch of aqua)
//   A  preset: red alert (red fade up/down, ~3.6 s)
//   W  preset: white strobe (controller FLASH mode at max QUICK)
//   Z  preset: zombie (mostly dark via DIY1, random WHITE/FLASH/QUICK strobe bursts)
//   z<dMin>,<dMax>,<bMin>,<bMax>\n  zombie dark/strobe timing in ms
//   g<ms>\n  gap after each brightness step (default 20)
//   x  stop the water/fade effect (lights stay on whatever color was last sent)
#include <Arduino.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <ir_NEC.h>

static const uint16_t kRecvPin = 16;   // RX2
static const uint16_t kSendPin = 17;   // TX2
static const uint16_t kCaptureBufferSize = 1024;
static const uint8_t  kCaptureTimeoutMs = 50; // long enough for NEC + repeat gaps
static const uint16_t kMinUnknownSize = 12;

IRrecv irrecv(kRecvPin, kCaptureBufferSize, kCaptureTimeoutMs, true);
IRsend irsend(kSendPin);
decode_results results;

static decode_type_t lastProto = UNKNOWN;
static uint64_t lastValue = 0;
static uint16_t lastBits = 0;
static uint16_t lastRaw[kCaptureBufferSize];
static uint16_t lastRawLen = 0;

static void sendNec(uint32_t code) {
  irrecv.disableIRIn();
  irsend.sendNEC(code, 32);
  irrecv.enableIRIn();
  Serial.printf("# sent NEC 0x%lX\n", (unsigned long)code);
}

// Swamp water: alternate GREEN/AQUA at random intervals so it never looks
// like a metronome. IR can't blend colors, so the randomness does the work.
static const uint32_t kGreen = 0xFF9A65;
static const uint32_t kAqua  = 0xFF8A75;
static uint32_t fadeBlue = 0xFF8A75;  // fade's accent breath; 'a<hex>' changes it
static uint32_t fadeMain = kGreen;    // fade's main color
static bool fadeAccent = true;        // false = main color only (red alert)
static bool waterOn = false;
static bool zombieOn = false;
static bool waterIsGreen = false;
static uint32_t waterMin = 1500, waterMax = 5000, waterNextAt = 0;

static void waterTick() {
  if (!waterOn || (int32_t)(millis() - waterNextAt) < 0) return;
  waterIsGreen = !waterIsGreen;
  uint32_t code = waterIsGreen ? kGreen : kAqua;
  irrecv.disableIRIn();
  irsend.sendNEC(code, 32);
  delay(40);
  irsend.sendNEC(code, 32);  // twice: a narrow beam + stray IR can eat one
  irrecv.enableIRIn();
  uint32_t dwell = random(waterMin, waterMax + 1);
  waterNextAt = millis() + dwell;
  Serial.printf("# water %s, next in %lu ms\n",
                waterIsGreen ? "GREEN" : "AQUA", (unsigned long)dwell);
}

// Swamp fade: brightness stepping via BRIGHT UP/DOWN. The controller clamps
// at min/max, so each dim floor is re-asserted with extra DOWNs. A lost or
// doubled IR frame can't make the level drift across cycles.
static const uint32_t kBrightUp   = 0xFF3AC5;  // captured
static const uint32_t kBrightDown = 0xFFBA45;  // standard 44-key table
static uint8_t  kFloorExtra = 1;
static uint16_t fastGapUs = 20000;  // trailing gap for brightness steps (NEC spec ~40ms min)
static bool fadeOn = false;
static uint16_t fadeSteps = 8, fadeStepMs = 250, fadeGreenCycles = 3;
static uint16_t fadeCycle = 0;   // 0..greenCycles-1 = green, greenCycles = aqua
static int32_t  fadePos = 0;     // index into the current breath
static uint32_t fadeNextAt = 0;

static void irOnce(uint32_t code) {
  irrecv.disableIRIn();
  irsend.sendNEC(code, 32);
  irrecv.enableIRIn();
}

// NEC frame with a shortened trailing gap (no 108 ms total-length padding).
// Brightness steps only: a controller that misses one is re-synced at the floor.
static void irFast(uint32_t code) {
  irrecv.disableIRIn();
  irsend.sendGeneric(kNecHdrMark, kNecHdrSpace, kNecBitMark, kNecOneSpace,
                     kNecBitMark, kNecZeroSpace, kNecBitMark, fastGapUs, 0,
                     code, 32, 38, true, 0, 33);
  irrecv.enableIRIn();
}

// One breath = [color x2] up*steps, hold, down*(steps+extra), hold.
// Aqua breath runs at double speed ("a touch o' blue").
static void fadeTick() {
  if (!fadeOn || (int32_t)(millis() - fadeNextAt) < 0) return;
  bool aqua = fadeAccent && (fadeCycle == fadeGreenCycles);
  uint16_t stepMs = aqua ? fadeStepMs / 2 : fadeStepMs;
  int32_t up = fadeSteps, down = fadeSteps + kFloorExtra;
  int32_t p = fadePos++;
  uint32_t wait = stepMs;

  if (p == 0) {                       // at the floor: set this breath's color
    irOnce(aqua ? fadeBlue : fadeMain); delay(40); irOnce(aqua ? fadeBlue : fadeMain);
    Serial.printf("# fade breath 0x%lX\n", (unsigned long)(aqua ? fadeBlue : fadeMain));
  } else if (p <= up) {
    irFast(kBrightUp);
    if (p == up) wait = stepMs * 3;    // brief hold at the top
  } else if (p <= up + down) {
    irFast(kBrightDown);
    if (p == up + down) {              // floor reached: next breath
      wait = stepMs * 2;
      fadePos = 0;
      fadeCycle = (fadeCycle + 1) % (fadeGreenCycles + 1);
    }
  }
  fadeNextAt = millis() + wait;
}

static void fadeStart() {
  waterOn = false; zombieOn = false;
  // Start from a known floor: main color, then DOWN past the bottom.
  irOnce(fadeMain);
  for (int i = 0; i < fadeSteps + kFloorExtra; i++) { delay(120); irOnce(kBrightDown); }
  fadeOn = true; fadeCycle = 0; fadePos = 0; fadeNextAt = millis() + 500;
  Serial.printf("# fade ON main 0x%lX accent %s, steps %u, stepMs %u, gap %u ms\n",
                (unsigned long)fadeMain, fadeAccent ? "on" : "off",
                fadeSteps, fadeStepMs, fastGapUs / 1000);
}

// Presets. The ESP-01S show node will map ESP-NOW cues onto these.
static void presetSwamp() {     // fast green breathing, a touch of aqua
  fadeMain = kGreen; fadeBlue = kAqua; fadeAccent = true;
  fadeSteps = 4; fadeStepMs = 0; fadeGreenCycles = 3; fastGapUs = 20000;
  fadeStart();
}
static void presetRedAlert() {  // slow red fade up and down, red only
  fadeMain = 0xFF1AE5; fadeAccent = false;
  fadeSteps = 10; fadeStepMs = 60; fastGapUs = 20000;  // Joe 09-30: faster, dim floor OK
  fadeStart();
}

// White strobe: the controller's own FLASH mode, pushed to max speed with
// QUICK. One-shot setup. The controller keeps strobing by itself, so nothing
// runs on a timer here and missed frames can't cause drift.
static const uint32_t kWhite = 0xFF22DD, kFlash = 0xFFD02F;
static const uint32_t kQuick = 0xFFE817;   // standard 44-key table, unverified
static void presetWhiteStrobe() {
  waterOn = false; fadeOn = false;
  irOnce(kWhite); delay(150); irOnce(kWhite); delay(150);
  for (int i = 0; i < 10; i++) { irFast(kBrightUp); }   // full brightness
  delay(150);
  irOnce(kFlash); delay(150); irOnce(kFlash); delay(300);
  for (int i = 0; i < 10; i++) { irOnce(kQuick); delay(60); }  // max speed
  Serial.println(F("# white strobe: WHITE, BRIGHT UP x10, FLASH, QUICK x10"));
}

// Zombie strobe: mostly DARK (DIY1, which Joe mixed to 0,0,0 on 09-30),
// with random short bursts of the controller's white FLASH strobe. Every
// command is idempotent, so a lost or doubled frame can't flip the pattern.
// POWER (a toggle) is deliberately never used.
static const uint32_t kDark = 0xFF30CF;    // DIY1 = black, verified 09-30
static bool zombieLit = false;
static uint32_t zDarkMin = 6000, zDarkMax = 15000, zBurstMin = 1500, zBurstMax = 4000;
static uint32_t zombieNextAt = 0;

// Burst = Joe's sequence: WHITE -> FLASH -> QUICK (controller strobes
// itself), then DIY1 dark. The ESP-made WHITE/DARK toggle strobe was tried
// 09-30 and read as a slow random flash, so the controller's FLASH is used.

static void zombieTick() {
  if (!zombieOn || (int32_t)(millis() - zombieNextAt) < 0) return;
  uint32_t dwell;
  if (zombieLit) {                       // burst over -> dark
    irOnce(kDark); delay(30); irOnce(kDark);
    dwell = random(zDarkMin, zDarkMax + 1);
  } else {                               // dark over -> strobe burst
    // Exactly the W preset timing (proven 09-30); tighter spacing (30 ms,
    // short-gap QUICKs) made the controller drop FLASH -> solid white.
    presetWhiteStrobe();
    dwell = random(zBurstMin, zBurstMax + 1);
  }
  zombieLit = !zombieLit;
  zombieNextAt = millis() + dwell;
  Serial.printf("# zombie %s for %lu ms\n", zombieLit ? "STROBE" : "dark",
                (unsigned long)dwell);
}

static void presetZombie() {
  waterOn = false; fadeOn = false;
  irOnce(kDark); delay(30); irOnce(kDark);
  zombieLit = false; zombieOn = true;
  zombieNextAt = millis() + random(zDarkMin, zDarkMax + 1);
  Serial.printf("# zombie ON: dark %lu-%lu ms, strobe %lu-%lu ms\n",
                (unsigned long)zDarkMin, (unsigned long)zDarkMax,
                (unsigned long)zBurstMin, (unsigned long)zBurstMax);
}

static void replay(bool forceRaw) {
  if (lastRawLen == 0) { Serial.println(F("# nothing captured yet")); return; }
  irrecv.disableIRIn();
  if (!forceRaw && lastProto != UNKNOWN &&
      irsend.send(lastProto, lastValue, lastBits)) {
    Serial.printf("# sent %s 0x%llX (%u bits)\n",
                  typeToString(lastProto).c_str(), lastValue, lastBits);
  } else {
    irsend.sendRaw(lastRaw, lastRawLen, 38);
    Serial.printf("# sent RAW (%u entries @38kHz)\n", lastRawLen);
  }
  irrecv.enableIRIn();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  randomSeed(esp_random());
  irrecv.setUnknownThreshold(kMinUnknownSize);
  irrecv.enableIRIn();
  irsend.begin();
  Serial.printf("\n# IR learner ready: RX GPIO%u, TX GPIO%u. "
                "Press remote buttons; 'r'/'R' = replay last.\n",
                kRecvPin, kSendPin);
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'r') replay(false);
    else if (c == 'R') replay(true);
    else if (c == 'n') {
      String hex = Serial.readStringUntil('\n');
      hex.trim();
      if (hex.length()) sendNec(strtoul(hex.c_str(), nullptr, 16));
    }
    else if (c == 'w') {
      String args = Serial.readStringUntil('\n');
      int comma = args.indexOf(',');
      if (comma > 0) {
        waterMin = args.substring(0, comma).toInt();
        waterMax = args.substring(comma + 1).toInt();
        if (waterMax < waterMin) waterMax = waterMin;
      }
      waterOn = true;
      waterNextAt = millis();
      Serial.printf("# water ON, dwell %lu-%lu ms\n",
                    (unsigned long)waterMin, (unsigned long)waterMax);
    }
    else if (c == 'f') {
      String args = Serial.readStringUntil('\n');
      int c1 = args.indexOf(','), c2 = args.indexOf(',', c1 + 1);
      if (c1 > 0 && c2 > c1) {
        fadeSteps = args.substring(0, c1).toInt();
        fadeStepMs = args.substring(c1 + 1, c2).toInt();
        fadeGreenCycles = args.substring(c2 + 1).toInt();
      }
      fadeStart();
    }
    else if (c == 'S') presetSwamp();
    else if (c == 'A') presetRedAlert();
    else if (c == 'W') presetWhiteStrobe();
    else if (c == 'Z') presetZombie();
    else if (c == 'z') {             // z<darkMin>,<darkMax>,<burstMin>,<burstMax>
      String a = Serial.readStringUntil('\n');
      uint32_t v[4]; int from = 0;
      for (int i = 0; i < 4; i++) {
        int comma = a.indexOf(',', from);
        v[i] = a.substring(from, comma < 0 ? a.length() : comma).toInt();
        from = comma + 1;
      }
      zDarkMin = v[0]; zDarkMax = v[1]; zBurstMin = v[2]; zBurstMax = v[3];
      Serial.printf("# zombie timing dark %lu-%lu, strobe %lu-%lu ms\n",
                    (unsigned long)v[0], (unsigned long)v[1],
                    (unsigned long)v[2], (unsigned long)v[3]);
    }
    else if (c == 'a') {             // a<hex>: color of the fade's blue breath
      fadeBlue = strtoul(Serial.readStringUntil('\n').c_str(), nullptr, 16);
      Serial.printf("# fade blue = 0x%lX\n", (unsigned long)fadeBlue);
    }
    else if (c == 'g') {             // g<ms>: brightness-step gap
      fastGapUs = Serial.readStringUntil('\n').toInt() * 1000;
      Serial.printf("# step gap %u ms\n", fastGapUs / 1000);
    }
    else if (c == 'x') { waterOn = false; fadeOn = false; zombieOn = false; Serial.println(F("# effects OFF")); }
  }

  waterTick();
  fadeTick();
  zombieTick();

  if (!irrecv.decode(&results)) return;

  // NEC repeat frames (button held) carry no code — note them, don't store.
  if (results.repeat) {
    Serial.println(F("# (repeat frame)"));
    irrecv.resume();
    return;
  }
  if (results.overflow)
    Serial.println(F("# WARNING: capture overflowed buffer"));

  Serial.println(F("----------------------------------------"));
  Serial.print(resultToHumanReadableBasic(&results));
  Serial.print(resultToSourceCode(&results));

  lastProto = results.decode_type;
  lastValue = results.value;
  lastBits = results.bits;
  uint16_t *raw = resultToRawArray(&results);
  lastRawLen = getCorrectedRawLength(&results);
  if (lastRawLen > kCaptureBufferSize) lastRawLen = kCaptureBufferSize;
  memcpy(lastRaw, raw, lastRawLen * sizeof(uint16_t));
  delete[] raw;

  irrecv.resume();
}
