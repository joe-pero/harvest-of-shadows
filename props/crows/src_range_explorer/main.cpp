#include <Arduino.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <ctype.h>

#include "espnow_compat.h"
#include "protocol.h"

constexpr size_t SERVO_COUNT = 4;
constexpr uint8_t SERVO_PINS[SERVO_COUNT] = {13, 12, 14, 27};
constexpr const char *SERVO_LABELS[SERVO_COUNT] = {
    "A_PAN",
    "A_TILT",
    "B_PAN",
    "B_TILT"};
constexpr int SERVO_MIN_US = 500;
constexpr int SERVO_MAX_US = 2400;
constexpr int ABSOLUTE_MIN_DEG = 0;
constexpr int ABSOLUTE_MAX_DEG = 180;
constexpr int DEFAULT_MARGIN_DEG = 2;
constexpr int DEFAULT_MIN_PER_SERVO[SERVO_COUNT] = {
    5,  // A_PAN head pan limit discovered during exploration
    ABSOLUTE_MIN_DEG + DEFAULT_MARGIN_DEG,
    10,  // B_PAN mirrored head pan (slightly higher start)
    ABSOLUTE_MIN_DEG + DEFAULT_MARGIN_DEG};
constexpr int DEFAULT_MAX_PER_SERVO[SERVO_COUNT] = {
    122,  // A_PAN upper limit
    ABSOLUTE_MAX_DEG - DEFAULT_MARGIN_DEG,
    122,  // B_PAN mirrored head pan
    ABSOLUTE_MAX_DEG - DEFAULT_MARGIN_DEG};
constexpr int HOME_ANGLE = 90;
constexpr int SWEEP_STEP = 5;
constexpr int SWEEP_DELAY_MS = 25;
constexpr int SCAN_DELAY_MS = 1000;  // 1 s between degree steps for slow scans
constexpr int TEST_PATTERN_DELAY_MS = 300;  // slower pacing for automated test
constexpr int TEST_PATTERN_STEP = 3;        // increase by ~3 deg per update

constexpr uint8_t MASTER_MAC[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  /* set your board MAC */;

constexpr size_t HEAD_COUNT = 2;
constexpr size_t HEAD_PAN_INDEX[HEAD_COUNT] = {0, 2};
constexpr size_t HEAD_TILT_INDEX[HEAD_COUNT] = {1, 3};
constexpr unsigned long SERVO_UPDATE_BASE_MIN = 30;   // ms between micro steps
constexpr unsigned long SERVO_UPDATE_BASE_MAX = 140;  // ms between micro steps
constexpr unsigned long MICRO_IDLE_JITTER_MS = 2500;  // occasional tiny adjustments

constexpr unsigned long ACTIVE_DECISION_MIN_MS = 900;
constexpr unsigned long ACTIVE_DECISION_MAX_MS = 2200;
constexpr unsigned long ACTIVE_MICRO_MIN_MS = 400;
constexpr unsigned long ACTIVE_MICRO_MAX_MS = 900;
constexpr unsigned long ACTIVE_HOLD_MS = 240000;  // remain lively for 4 minutes

constexpr unsigned long AMBIENT_BURST_GAP_MIN_MS = 270000;  // ~4.5 min
constexpr unsigned long AMBIENT_BURST_GAP_MAX_MS = 330000;  // ~5.5 min
constexpr unsigned long AMBIENT_STAGE_HOLD_MIN_MS = 1200;
constexpr unsigned long AMBIENT_STAGE_HOLD_MAX_MS = 3500;

Servo servos[SERVO_COUNT];
int servoAngles[SERVO_COUNT] = {HOME_ANGLE, HOME_ANGLE, HOME_ANGLE, HOME_ANGLE};
int servoMinDeg[SERVO_COUNT] = {
    DEFAULT_MIN_PER_SERVO[0],
    DEFAULT_MIN_PER_SERVO[1],
    DEFAULT_MIN_PER_SERVO[2],
    DEFAULT_MIN_PER_SERVO[3]};
int servoMaxDeg[SERVO_COUNT] = {
    DEFAULT_MAX_PER_SERVO[0],
    DEFAULT_MAX_PER_SERVO[1],
    DEFAULT_MAX_PER_SERVO[2],
    DEFAULT_MAX_PER_SERVO[3]};
constexpr bool SERVO_MIRRORED[SERVO_COUNT] = {
    false,  // A_PAN
    false,  // A_TILT
    true,   // B_PAN (physical orientation mirrored)
    false   // B_TILT (can adjust later if required)
};
String inputLine;

void printStatus();
void printHelp();
void printLimits();
void sweepServo(size_t index);
void sweepAll();
void scanServoSlow(size_t index);
void scanAllSlow();
void runTestPattern(size_t startIndex);
void runSingleTestPattern(size_t index);
void processCommand(const String &line);
int servoIndexFromToken(const String &token);
bool parseAngle(const String &text, int &value);
void setServoAngle(size_t index, int requestedAngle, bool verbose);
void applyServoLimits(size_t index, int newMin, int newMax, bool verbose);
void resetServoLimits(size_t index, bool verbose);
void resetAllLimits(bool verbose);
void initializeAutomation();
void updateAutomation(unsigned long now);

enum class BehaviorMode : uint8_t { Ambient = 0, Active = 1 };

unsigned long randomBetween(unsigned long minValue, unsigned long maxValue);

struct ServoMotion {
  int target = HOME_ANGLE;
  unsigned long nextStepMs = 0;
  unsigned long minIntervalMs = SERVO_UPDATE_BASE_MIN;
  unsigned long maxIntervalMs = SERVO_UPDATE_BASE_MAX;
  uint8_t stepSize = 1;
};

struct Pose {
  int pan = HOME_ANGLE;
  int tilt = HOME_ANGLE;
  unsigned long holdMs = 1000;
};

struct BurstState {
  bool active = false;
  uint8_t stageIndex = 0;
  uint8_t stageCount = 0;
  Pose stages[5];
  unsigned long holdUntilMs = 0;
  bool holdArmed = false;
};

struct HeadState {
  size_t panIndex = 0;
  size_t tiltIndex = 0;
  int neutralPan = HOME_ANGLE;
  int neutralTilt = HOME_ANGLE;
  unsigned long nextDecisionMs = 0;
  unsigned long nextMicroMs = 0;
  unsigned long nextAmbientBurstMs = 0;
  BurstState ambient;
};

ServoMotion gMotion[SERVO_COUNT];
HeadState gHeads[HEAD_COUNT];
BehaviorMode gMode = BehaviorMode::Ambient;
Zone gActiveZone = ZNONE;
unsigned long gActiveUntilMs = 0;

uint32_t gLastSession = 0;
uint32_t gLastRxReq = 0;

void setBehaviorMode(BehaviorMode mode, unsigned long now);
void planActiveTarget(size_t headIndex, unsigned long now);
void maybeScheduleMicro(size_t headIndex, unsigned long now);
void startAmbientBurst(size_t headIndex, unsigned long now);
void activateBurstStage(HeadState &h, unsigned long now);
void updateHeadBehavior(size_t headIndex, unsigned long now);
void queueServoTarget(size_t index, int target, unsigned long minIntervalMs, unsigned long maxIntervalMs, uint8_t stepSize);
void updateServoMotion(size_t index, unsigned long now);
void initRadio();
void handleRecvCommon(const uint8_t *mac, const uint8_t *data, int len);
uint16_t crc16_ccitt(const uint8_t *data, size_t len);
void refreshHeadBounds();

unsigned long randomBetween(unsigned long minValue, unsigned long maxValue) {
  if (maxValue <= minValue) {
    return minValue;
  }
  unsigned long span = maxValue - minValue;
  long spanL = static_cast<long>(span);
  return minValue + static_cast<unsigned long>(random(spanL + 1));
}

int toPhysicalAngle(size_t index, int logicalAngle) {
  if (index >= SERVO_COUNT) {
    return logicalAngle;
  }
  if (!SERVO_MIRRORED[index]) {
    return logicalAngle;
  }
  return ABSOLUTE_MAX_DEG - logicalAngle;
}

void queueServoTarget(size_t index, int target, unsigned long minIntervalMs, unsigned long maxIntervalMs, uint8_t stepSize) {
  if (index >= SERVO_COUNT) {
    return;
  }
  if (minIntervalMs > maxIntervalMs) {
    unsigned long tmp = minIntervalMs;
    minIntervalMs = maxIntervalMs;
    maxIntervalMs = tmp;
  }
  target = constrain(target, servoMinDeg[index], servoMaxDeg[index]);
  ServoMotion &m = gMotion[index];
  m.target = target;
  m.minIntervalMs = minIntervalMs;
  m.maxIntervalMs = maxIntervalMs;
  m.stepSize = stepSize == 0 ? 1 : stepSize;
  unsigned long now = millis();
  m.nextStepMs = now + randomBetween(minIntervalMs, maxIntervalMs);
}

void initializeAutomation() {
  unsigned long now = millis();
  for (size_t i = 0; i < SERVO_COUNT; ++i) {
    gMotion[i].target = servoAngles[i];
    gMotion[i].minIntervalMs = SERVO_UPDATE_BASE_MIN;
    gMotion[i].maxIntervalMs = SERVO_UPDATE_BASE_MAX;
    gMotion[i].stepSize = 1;
    gMotion[i].nextStepMs = now + randomBetween(SERVO_UPDATE_BASE_MIN, SERVO_UPDATE_BASE_MAX);
  }

  for (size_t i = 0; i < HEAD_COUNT; ++i) {
    HeadState &h = gHeads[i];
    h.panIndex = HEAD_PAN_INDEX[i];
    h.tiltIndex = HEAD_TILT_INDEX[i];
    h.neutralPan = (servoMinDeg[h.panIndex] + servoMaxDeg[h.panIndex]) / 2;
    h.neutralTilt = (servoMinDeg[h.tiltIndex] + servoMaxDeg[h.tiltIndex]) / 2;
    h.ambient = BurstState{};
    h.nextDecisionMs = now + randomBetween(ACTIVE_DECISION_MIN_MS, ACTIVE_DECISION_MAX_MS);
    h.nextMicroMs = now + randomBetween(ACTIVE_MICRO_MIN_MS, ACTIVE_MICRO_MAX_MS);
    h.nextAmbientBurstMs = now + randomBetween(AMBIENT_BURST_GAP_MIN_MS, AMBIENT_BURST_GAP_MAX_MS);

    setServoAngle(h.panIndex, h.neutralPan, false);
    setServoAngle(h.tiltIndex, h.neutralTilt, false);
    queueServoTarget(h.panIndex, h.neutralPan, 140, 220, 1);
    queueServoTarget(h.tiltIndex, h.neutralTilt, 160, 240, 1);
  }

  gMode = BehaviorMode::Ambient;
  gActiveZone = ZNONE;
  gActiveUntilMs = 0;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println(F("Crows servo range explorer"));
  Serial.println(F("---------------------------"));

  for (int i = 0; i < 4; ++i) {
    ESP32PWM::allocateTimer(i);
  }

  for (size_t i = 0; i < SERVO_COUNT; ++i) {
    servos[i].setPeriodHertz(50);  // Standard hobby servo update rate
    servos[i].attach(SERVO_PINS[i], SERVO_MIN_US, SERVO_MAX_US);
    setServoAngle(i, HOME_ANGLE, false);
  }

  uint32_t seed = (uint32_t)esp_random() ^ (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFFull) ^ millis();
  randomSeed(seed);

  initializeAutomation();
  initRadio();
  printStatus();
  printHelp();
}

void loop() {
  while (Serial.available() > 0) {
    char ch = static_cast<char>(Serial.read());
    if (ch == '\r') {
      continue;  // Ignore carriage returns
    }
    if (ch == '\n') {
      processCommand(inputLine);
      inputLine = "";
    } else if (inputLine.length() < 64) {
      inputLine += ch;
    }
  }

  updateAutomation(millis());
}

void updateServoMotion(size_t index, unsigned long now) {
  if (index >= SERVO_COUNT) {
    return;
  }
  ServoMotion &m = gMotion[index];
  if (now < m.nextStepMs) {
    return;
  }
  int current = servoAngles[index];
  if (current == m.target) {
    m.nextStepMs = now + randomBetween(m.minIntervalMs, m.maxIntervalMs);
    return;
  }

  int diff = m.target - current;
  int step = m.stepSize;
  if (abs(diff) < step) {
    step = abs(diff);
  }
  if (step == 0) {
    step = diff >= 0 ? 1 : -1;
  }
  if (diff < 0) {
    step = -step;
  }
  setServoAngle(index, current + step, false);

  m.nextStepMs = now + randomBetween(m.minIntervalMs, m.maxIntervalMs);
}

void activateBurstStage(HeadState &h, unsigned long now) {
  BurstState &b = h.ambient;
  if (b.stageIndex >= b.stageCount) {
    b.active = false;
    return;
  }
  Pose &p = b.stages[b.stageIndex];
  queueServoTarget(h.panIndex, p.pan, 160, 280, 1);
  queueServoTarget(h.tiltIndex, p.tilt, 170, 300, 1);
  b.holdArmed = false;
  b.holdUntilMs = 0;
}

void startAmbientBurst(size_t headIndex, unsigned long now) {
  if (headIndex >= HEAD_COUNT) {
    return;
  }
  HeadState &h = gHeads[headIndex];
  BurstState &b = h.ambient;
  b = BurstState{};
  b.active = true;
  b.stageCount = 5;
  b.stageIndex = 0;

  int panMin = servoMinDeg[h.panIndex];
  int panMax = servoMaxDeg[h.panIndex];
  int panRange = panMax - panMin;
  int panThird = panMin + panRange / 3;
  int panTwoThird = panMin + (2 * panRange) / 3;

  int tiltMin = servoMinDeg[h.tiltIndex];
  int tiltMax = servoMaxDeg[h.tiltIndex];

  bool startLeft = random(0, 2) == 0;
  b.stages[0].pan = startLeft ? panMin : panMax;
  b.stages[0].tilt = h.neutralTilt + random(-6, 7);
  b.stages[0].tilt = constrain(b.stages[0].tilt, tiltMin, tiltMax);
  b.stages[0].holdMs = randomBetween(AMBIENT_STAGE_HOLD_MIN_MS + 600, AMBIENT_STAGE_HOLD_MAX_MS + 1000);

  bool midRange = random(0, 2) == 0;
  b.stages[1].pan = midRange ? (panMin + panMax) / 2 : (startLeft ? panTwoThird : panThird);
  b.stages[1].tilt = h.neutralTilt + random(-10, 11);
  b.stages[1].tilt = constrain(b.stages[1].tilt, tiltMin, tiltMax);
  b.stages[1].holdMs = randomBetween(AMBIENT_STAGE_HOLD_MIN_MS, AMBIENT_STAGE_HOLD_MAX_MS);

  b.stages[2].pan = startLeft ? panMax : panMin;
  b.stages[2].tilt = h.neutralTilt + random(-6, 7);
  b.stages[2].tilt = constrain(b.stages[2].tilt, tiltMin, tiltMax);
  b.stages[2].holdMs = randomBetween(AMBIENT_STAGE_HOLD_MIN_MS + 400, AMBIENT_STAGE_HOLD_MAX_MS + 1200);

  b.stages[3].pan = h.neutralPan + random(-3, 4);
  b.stages[3].pan = constrain(b.stages[3].pan, panMin, panMax);
  int tiltChoice = random(0, 3);
  if (tiltChoice == 0) {
    b.stages[3].tilt = tiltMin + 3;
  } else if (tiltChoice == 1) {
    b.stages[3].tilt = tiltMax - 3;
  } else {
    b.stages[3].tilt = h.neutralTilt + random(-8, 9);
    b.stages[3].tilt = constrain(b.stages[3].tilt, tiltMin, tiltMax);
  }
  b.stages[3].holdMs = randomBetween(AMBIENT_STAGE_HOLD_MIN_MS + 500, AMBIENT_STAGE_HOLD_MAX_MS + 1500);

  b.stages[4].pan = h.neutralPan;
  b.stages[4].tilt = h.neutralTilt;
  b.stages[4].holdMs = randomBetween(AMBIENT_STAGE_HOLD_MIN_MS + 700, AMBIENT_STAGE_HOLD_MAX_MS + 1600);

  activateBurstStage(h, now);
  h.nextAmbientBurstMs = now + randomBetween(AMBIENT_BURST_GAP_MIN_MS, AMBIENT_BURST_GAP_MAX_MS);
}

void maybeScheduleMicro(size_t headIndex, unsigned long now) {
  if (headIndex >= HEAD_COUNT) {
    return;
  }
  HeadState &h = gHeads[headIndex];
  if (now < h.nextMicroMs) {
    return;
  }

  int panIdx = h.panIndex;
  int tiltIdx = h.tiltIndex;
  if (abs(servoAngles[panIdx] - gMotion[panIdx].target) <= 1 &&
      abs(servoAngles[tiltIdx] - gMotion[tiltIdx].target) <= 1) {
    int panTarget = constrain(gMotion[panIdx].target + random(-1, 2), servoMinDeg[panIdx], servoMaxDeg[panIdx]);
    int tiltTarget = constrain(gMotion[tiltIdx].target + random(-2, 3), servoMinDeg[tiltIdx], servoMaxDeg[tiltIdx]);
    queueServoTarget(panIdx, panTarget, 110, 200, 1);
    queueServoTarget(tiltIdx, tiltTarget, 110, 210, 1);
  }

  h.nextMicroMs = now + randomBetween(ACTIVE_MICRO_MIN_MS, ACTIVE_MICRO_MAX_MS);
}

void planActiveTarget(size_t headIndex, unsigned long now) {
  if (headIndex >= HEAD_COUNT) {
    return;
  }
  HeadState &h = gHeads[headIndex];
  int panIdx = h.panIndex;
  int tiltIdx = h.tiltIndex;
  int panMin = servoMinDeg[panIdx];
  int panMax = servoMaxDeg[panIdx];
  int panRange = panMax - panMin;
  int panThird = panMin + panRange / 3;
  int panTwoThird = panMin + (2 * panRange) / 3;

  int panTarget;
  int roll = random(100);
  if (roll < 30) {
    panTarget = (random(0, 2) == 0) ? panMin : panMax;
  } else if (roll < 60) {
    panTarget = (random(0, 2) == 0) ? panThird : panTwoThird;
    panTarget += random(-6, 7);
  } else {
    panTarget = h.neutralPan + random(-panRange / 6, panRange / 6 + 1);
  }
  panTarget = constrain(panTarget, panMin, panMax);

  int tiltIdxMin = servoMinDeg[tiltIdx];
  int tiltIdxMax = servoMaxDeg[tiltIdx];
  int tiltTarget;
  int tiltRoll = random(100);
  if (tiltRoll < 35) {
    tiltTarget = tiltIdxMin + random(2, 6);
  } else if (tiltRoll < 70) {
    tiltTarget = tiltIdxMax - random(2, 6);
  } else {
    tiltTarget = h.neutralTilt + random(-12, 13);
  }
  tiltTarget = constrain(tiltTarget, tiltIdxMin, tiltIdxMax);

  queueServoTarget(panIdx, panTarget, 70, 140, 1);
  queueServoTarget(tiltIdx, tiltTarget, 60, 120, 1);
  h.nextDecisionMs = now + randomBetween(ACTIVE_DECISION_MIN_MS, ACTIVE_DECISION_MAX_MS);
}

void updateHeadBehavior(size_t headIndex, unsigned long now) {
  if (headIndex >= HEAD_COUNT) {
    return;
  }
  HeadState &h = gHeads[headIndex];

  if (gMode == BehaviorMode::Active) {
    h.ambient.active = false;
    if (now >= h.nextDecisionMs) {
      planActiveTarget(headIndex, now);
    }
    maybeScheduleMicro(headIndex, now);
  } else {
    if (h.ambient.active) {
      BurstState &b = h.ambient;
      if (!b.holdArmed) {
        Pose &p = b.stages[b.stageIndex];
        if (abs(servoAngles[h.panIndex] - p.pan) <= 1 &&
            abs(servoAngles[h.tiltIndex] - p.tilt) <= 1) {
          b.holdArmed = true;
          b.holdUntilMs = now + p.holdMs;
        }
      } else if (now >= b.holdUntilMs) {
        b.stageIndex++;
        if (b.stageIndex >= b.stageCount) {
          b.active = false;
        } else {
          activateBurstStage(h, now);
        }
      }
    } else {
      if (now >= h.nextAmbientBurstMs) {
        startAmbientBurst(headIndex, now);
      } else {
        int panTarget = h.neutralPan;
        int tiltTarget = h.neutralTilt;
        if (gMotion[h.panIndex].target != panTarget ||
            abs(servoAngles[h.panIndex] - panTarget) > 2) {
          queueServoTarget(h.panIndex, panTarget, 180, 260, 1);
        }
        if (gMotion[h.tiltIndex].target != tiltTarget ||
            abs(servoAngles[h.tiltIndex] - tiltTarget) > 2) {
          queueServoTarget(h.tiltIndex, tiltTarget, 200, 280, 1);
        }
      }
    }
  }
}

void updateAutomation(unsigned long now) {
  for (size_t i = 0; i < HEAD_COUNT; ++i) {
    updateHeadBehavior(i, now);
  }
  for (size_t i = 0; i < SERVO_COUNT; ++i) {
    updateServoMotion(i, now);
  }
  if (gMode == BehaviorMode::Active && gActiveUntilMs > 0 && now >= gActiveUntilMs) {
    gActiveZone = ZNONE;
    gActiveUntilMs = 0;
    setBehaviorMode(BehaviorMode::Ambient, now);
  }
}

void setBehaviorMode(BehaviorMode mode, unsigned long now) {
  if (gMode == mode) {
    return;
  }
  gMode = mode;
  if (mode == BehaviorMode::Active) {
    for (size_t i = 0; i < HEAD_COUNT; ++i) {
      HeadState &h = gHeads[i];
      h.ambient.active = false;
      h.nextDecisionMs = now;
      h.nextMicroMs = now + randomBetween(ACTIVE_MICRO_MIN_MS, ACTIVE_MICRO_MAX_MS);
    }
    Serial.println(F("[MODE] ACTIVE attention"));
  } else {
    for (size_t i = 0; i < HEAD_COUNT; ++i) {
      HeadState &h = gHeads[i];
      h.ambient.active = false;
      h.nextAmbientBurstMs = now + randomBetween(AMBIENT_BURST_GAP_MIN_MS, AMBIENT_BURST_GAP_MAX_MS);
      queueServoTarget(h.panIndex, h.neutralPan, 200, 320, 1);
      queueServoTarget(h.tiltIndex, h.neutralTilt, 210, 320, 1);
    }
    Serial.println(F("[MODE] AMBIENT idle"));
  }
}

void refreshHeadBounds() {
  for (size_t i = 0; i < HEAD_COUNT; ++i) {
    HeadState &h = gHeads[i];
    h.neutralPan = (servoMinDeg[h.panIndex] + servoMaxDeg[h.panIndex]) / 2;
    h.neutralTilt = (servoMinDeg[h.tiltIndex] + servoMaxDeg[h.tiltIndex]) / 2;
  }
}

uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
  }
  return crc;
}

void handleRecvCommon(const uint8_t *mac, const uint8_t *data, int len) {
  (void)mac;
  if (len != (int)sizeof(Msg)) {
    return;
  }
  Msg in{};
  memcpy(&in, data, sizeof(Msg));
  if (in.magic != MSG_MAGIC || in.version != MSG_VER) {
    return;
  }
  uint16_t crc = crc16_ccitt(reinterpret_cast<const uint8_t *>(&in), sizeof(Msg) - 2);
  if (crc != in.crc16) {
    return;
  }

  if (in.session_id < gLastSession) {
    return;
  }
  if (in.session_id == gLastSession && in.request_id <= gLastRxReq) {
    return;
  }
  if (in.session_id > gLastSession) {
    gLastSession = in.session_id;
    gLastRxReq = 0;
  }
  gLastRxReq = in.request_id;

  unsigned long now = millis();
  Zone zone = static_cast<Zone>(in.zone);

  if (in.type == START) {
    if (zone == ZFAR) {
      gActiveZone = ZFAR;
      gActiveUntilMs = now + ACTIVE_HOLD_MS;
      setBehaviorMode(BehaviorMode::Active, now);
      Serial.println(F("[RADIO] FAR START -> ACTIVE"));
    } else if (zone == ZNONE) {
      gActiveZone = ZNONE;
      gActiveUntilMs = 0;
      setBehaviorMode(BehaviorMode::Ambient, now);
      Serial.println(F("[RADIO] AMBIENT START"));
    }
  } else if (in.type == END_) {
    if (zone == ZFAR || zone == ZNONE || gActiveZone == zone) {
      gActiveZone = ZNONE;
      gActiveUntilMs = 0;
      setBehaviorMode(BehaviorMode::Ambient, now);
      Serial.println(F("[RADIO] ZONE END -> AMBIENT"));
    }
  }
}

void initRadio() {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
#ifdef ESPNOW_CHANNEL
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
#endif
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("ESP-NOW init failed"));
    return;
  }
  espnow_register_recv_bridge(handleRecvCommon);
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, MASTER_MAC, 6);
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void setServoAngle(size_t index, int requestedAngle, bool verbose) {
  if (index >= SERVO_COUNT) {
    return;
  }
  int clamped = constrain(requestedAngle, servoMinDeg[index], servoMaxDeg[index]);
  servoAngles[index] = clamped;
  servos[index].write(toPhysicalAngle(index, clamped));
  if (verbose) {
    Serial.printf("[%u/%s] %d deg", static_cast<unsigned>(index), SERVO_LABELS[index], clamped);
    if (clamped != requestedAngle) {
      Serial.printf(" (clamped from %d)", requestedAngle);
    }
    Serial.println();
  }
}

void printStatus() {
  Serial.println();
  unsigned long now = millis();
  Serial.printf("Mode: %s (zone=%s)\n",
                gMode == BehaviorMode::Active ? "ACTIVE" : "AMBIENT",
                zname(gActiveZone));
  if (gMode == BehaviorMode::Active && gActiveUntilMs > now) {
    unsigned long remaining = (gActiveUntilMs - now) / 1000UL;
    Serial.printf("  Active window remaining ~%lu s\n", remaining);
  }
  Serial.println(F("Current servo targets:"));
  for (size_t i = 0; i < SERVO_COUNT; ++i) {
    Serial.printf("  [%u] %-6s on GPIO %u -> %3d deg (limits %3d-%3d)\n",
                  static_cast<unsigned>(i), SERVO_LABELS[i], SERVO_PINS[i], servoAngles[i],
                  servoMinDeg[i], servoMaxDeg[i]);
  }
  Serial.println();
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  HELP                Show this help"));
  Serial.println(F("  STATUS              Print current angles"));
  Serial.println(F("  LIMITS              Show per-servo limits"));
  Serial.println(F("  LIMIT <servo> <min> <max>  Update a servo's limits (deg)"));
  Serial.println(F("  LIMIT <servo> RESET Reset a servo to default limits"));
  Serial.println(F("  LIMIT RESET         Reset all servos to default limits"));
  Serial.println(F("  SWEEP               Sweep every servo"));
  Serial.println(F("  SWEEP <servo>       Sweep one servo (e.g. SWEEP A_PAN)"));
  Serial.println(F("  SCAN                Slow-scan every servo (~1s/deg)"));
  Serial.println(F("  SCAN <servo>        Slow-scan one servo for limit finding"));
  Serial.println(F("  TEST                Run automated test pattern on all servos"));
  Serial.println(F("  TEST <servo>        Run automated test pattern on one servo"));
  Serial.println(F("  <servo> <angle>     Move servo to angle (e.g. A_TILT 110)"));
  Serial.println(F("  <servo> SWEEP       Sweep one servo (e.g. 2 SWEEP)"));
  Serial.println(F("  <servo>             Shortcut: sweep servo by id (e.g. 3)"));
  Serial.println(F("Servo ids: 0=A_PAN, 1=A_TILT, 2=B_PAN, 3=B_TILT"));
  Serial.println(F("Default guard rails:"));
  for (size_t i = 0; i < SERVO_COUNT; ++i) {
    Serial.printf("    %-6s %3d-%3d deg\n", SERVO_LABELS[i],
                  DEFAULT_MIN_PER_SERVO[i], DEFAULT_MAX_PER_SERVO[i]);
  }
  Serial.println(F("Automation: FAR zone START keeps heads alert for ~4 min; ambient mode fires a gaze burst every ~5 min."));
  Serial.printf("Defaults keep ~%d deg margin from the absolute stops.\n", DEFAULT_MARGIN_DEG);
  Serial.println();
}

void printLimits() {
  Serial.println();
  Serial.println(F("Servo limits (deg):"));
  for (size_t i = 0; i < SERVO_COUNT; ++i) {
    Serial.printf("  [%u/%s] %3d-%3d\n", static_cast<unsigned>(i), SERVO_LABELS[i],
                  servoMinDeg[i], servoMaxDeg[i]);
  }
  Serial.println(F("Defaults:"));
  for (size_t i = 0; i < SERVO_COUNT; ++i) {
    Serial.printf("    %-6s %3d-%3d\n", SERVO_LABELS[i],
                  DEFAULT_MIN_PER_SERVO[i], DEFAULT_MAX_PER_SERVO[i]);
  }
  Serial.printf("Absolute guard: %d-%d (defaults leave %d deg headroom each side)\n",
                ABSOLUTE_MIN_DEG, ABSOLUTE_MAX_DEG, DEFAULT_MARGIN_DEG);
  Serial.println();
}

void sweepServo(size_t index) {
  if (index >= SERVO_COUNT) {
    return;
  }
  Serial.printf("Sweeping [%u/%s] %d-%d deg\n", static_cast<unsigned>(index), SERVO_LABELS[index],
                servoMinDeg[index], servoMaxDeg[index]);
  int original = servoAngles[index];
  for (int angle = servoMinDeg[index]; angle <= servoMaxDeg[index]; angle += SWEEP_STEP) {
    servos[index].write(toPhysicalAngle(index, angle));
    delay(SWEEP_DELAY_MS);
  }
  for (int angle = servoMaxDeg[index]; angle >= servoMinDeg[index]; angle -= SWEEP_STEP) {
    servos[index].write(toPhysicalAngle(index, angle));
    delay(SWEEP_DELAY_MS);
  }
  setServoAngle(index, original, false);
  Serial.printf("[%u/%s] back to %d deg\n", static_cast<unsigned>(index), SERVO_LABELS[index], original);
}

void sweepAll() {
  for (size_t i = 0; i < SERVO_COUNT; ++i) {
    sweepServo(i);
  }
}

void scanServoSlow(size_t index) {
  if (index >= SERVO_COUNT) {
    return;
  }

  int minDeg = servoMinDeg[index];
  int maxDeg = servoMaxDeg[index];
  if (minDeg >= maxDeg) {
    Serial.printf("[%u/%s] scan skipped: min >= max (%d >= %d)\n",
                  static_cast<unsigned>(index), SERVO_LABELS[index], minDeg, maxDeg);
    return;
  }

  Serial.printf("Scanning [%u/%s] %d-%d deg (~1s/deg)\n",
                static_cast<unsigned>(index), SERVO_LABELS[index], minDeg, maxDeg);
  int original = servoAngles[index];

  for (int angle = minDeg; angle <= maxDeg; ++angle) {
    servos[index].write(toPhysicalAngle(index, angle));
    servoAngles[index] = angle;
    Serial.printf("SCAN,%s,%03d\n", SERVO_LABELS[index], angle);
    delay(SCAN_DELAY_MS);
  }
  for (int angle = maxDeg - 1; angle >= minDeg; --angle) {
    servos[index].write(toPhysicalAngle(index, angle));
    servoAngles[index] = angle;
    Serial.printf("SCAN,%s,%03d\n", SERVO_LABELS[index], angle);
    delay(SCAN_DELAY_MS);
  }

  setServoAngle(index, original, false);
  Serial.printf("[%u/%s] back to %d deg\n",
                static_cast<unsigned>(index), SERVO_LABELS[index], original);
}

void scanAllSlow() {
  for (size_t i = 0; i < SERVO_COUNT; ++i) {
    scanServoSlow(i);
  }
}

void runSingleTestPattern(size_t index) {
  if (index >= SERVO_COUNT) {
    return;
  }
  int minDeg = servoMinDeg[index];
  int maxDeg = servoMaxDeg[index];
  if (minDeg >= maxDeg) {
    Serial.printf("[%u/%s] test skipped: min >= max (%d >= %d)\n",
                  static_cast<unsigned>(index), SERVO_LABELS[index], minDeg, maxDeg);
    return;
  }

  Serial.printf("TEST,%s,START,%d,%d\n", SERVO_LABELS[index], minDeg, maxDeg);
  int original = servoAngles[index];

  for (int angle = minDeg; angle <= maxDeg; angle += TEST_PATTERN_STEP) {
    servos[index].write(toPhysicalAngle(index, angle));
    servoAngles[index] = angle;
    Serial.printf("TEST,%s,%03d\n", SERVO_LABELS[index], angle);
    delay(TEST_PATTERN_DELAY_MS);
  }
  for (int angle = maxDeg; angle >= minDeg; angle -= TEST_PATTERN_STEP) {
    servos[index].write(toPhysicalAngle(index, angle));
    servoAngles[index] = angle;
    Serial.printf("TEST,%s,%03d\n", SERVO_LABELS[index], angle);
    delay(TEST_PATTERN_DELAY_MS);
  }

  setServoAngle(index, original, false);
  Serial.printf("TEST,%s,END,%d\n", SERVO_LABELS[index], original);
}

void runTestPattern(size_t startIndex) {
  if (startIndex >= SERVO_COUNT) {
    startIndex = 0;
  }
  for (size_t i = startIndex; i < SERVO_COUNT; ++i) {
    runSingleTestPattern(i);
  }
}

void processCommand(const String &line) {
  String trimmed = line;
  trimmed.trim();
  if (trimmed.length() == 0) {
    return;
  }

  String upper = trimmed;
  upper.toUpperCase();

  if (upper == "HELP" || upper == "?") {
    printHelp();
    return;
  }
  if (upper == "STATUS") {
    printStatus();
    return;
  }
  if (upper == "LIMITS") {
    printLimits();
    return;
  }
  if (upper == "LIMIT RESET") {
    resetAllLimits(true);
    printLimits();
    return;
  }
  if (upper == "SCAN") {
    scanAllSlow();
    return;
  }
  if (upper == "TEST") {
    runTestPattern(0);
    return;
  }
  if (upper == "SWEEP") {
    sweepAll();
    return;
  }

  int spaceIndex = trimmed.indexOf(' ');
  if (spaceIndex < 0) {
    int index = servoIndexFromToken(trimmed);
    if (index >= 0) {
      sweepServo(static_cast<size_t>(index));
    } else {
      Serial.println(F("Unknown command. Type HELP for a list of options."));
    }
    return;
  }

  String first = trimmed.substring(0, spaceIndex);
  String rest = trimmed.substring(spaceIndex + 1);
  first.trim();
  rest.trim();

  if (first.length() == 0) {
    Serial.println(F("Missing command before argument."));
    return;
  }

  String upperFirst = first;
  upperFirst.toUpperCase();

  if (upperFirst == "LIMIT") {
    if (rest.length() == 0) {
      Serial.println(F("Usage: LIMIT <servo> <min> <max> | LIMIT <servo> RESET | LIMIT RESET"));
      return;
    }
    String limitArgs = rest;
    limitArgs.trim();
    String upperArgs = limitArgs;
    upperArgs.toUpperCase();
    if (upperArgs == "RESET") {
      resetAllLimits(true);
      return;
    }

    int argSpace = limitArgs.indexOf(' ');
    if (argSpace < 0) {
      Serial.println(F("LIMIT command requires a servo id."));
      return;
    }
    String servoToken = limitArgs.substring(0, argSpace);
    servoToken.trim();
    String remainder = limitArgs.substring(argSpace + 1);
    remainder.trim();
    if (servoToken.length() == 0 || remainder.length() == 0) {
      Serial.println(F("LIMIT command requires a servo id and arguments."));
      return;
    }

    int index = servoIndexFromToken(servoToken);
    if (index < 0) {
      Serial.println(F("Unknown servo id for LIMIT command."));
      return;
    }

    String upperRemainder = remainder;
    upperRemainder.toUpperCase();
    if (upperRemainder == "RESET") {
      resetServoLimits(static_cast<size_t>(index), true);
      return;
    }

    int valueSpace = remainder.indexOf(' ');
    if (valueSpace < 0) {
      Serial.println(F("LIMIT command requires both min and max in degrees."));
      return;
    }
    String minText = remainder.substring(0, valueSpace);
    String maxText = remainder.substring(valueSpace + 1);
    minText.trim();
    maxText.trim();
    if (minText.length() == 0 || maxText.length() == 0) {
      Serial.println(F("LIMIT command requires both min and max in degrees."));
      return;
    }

    int newMin = 0;
    int newMax = 0;
    if (!parseAngle(minText, newMin) || !parseAngle(maxText, newMax)) {
      Serial.println(F("LIMIT min/max must be whole numbers."));
      return;
    }
    if (newMin >= newMax) {
      Serial.println(F("LIMIT min must be less than max."));
      return;
    }

    applyServoLimits(static_cast<size_t>(index), newMin, newMax, true);
    return;
  }

  if (upperFirst == "SCAN") {
    if (rest.length() == 0) {
      Serial.println(F("Usage: SCAN <servo> (or SCAN to scan all)"));
      return;
    }
    int index = servoIndexFromToken(rest);
    if (index >= 0) {
      scanServoSlow(static_cast<size_t>(index));
    } else {
      Serial.println(F("Unknown servo id for scan."));
    }
    return;
  }

  if (upperFirst == "TEST") {
    int index = servoIndexFromToken(rest);
    if (index >= 0) {
      runSingleTestPattern(static_cast<size_t>(index));
    } else {
      Serial.println(F("Unknown servo id for test pattern."));
    }
    return;
  }

  if (upperFirst == "SWEEP") {
    int index = servoIndexFromToken(rest);
    if (index >= 0) {
      sweepServo(static_cast<size_t>(index));
    } else {
      Serial.println(F("Unknown servo id for sweep."));
    }
    return;
  }

  int index = servoIndexFromToken(first);
  if (index < 0) {
    Serial.println(F("Unknown servo id. Type HELP for the list."));
    return;
  }

  String upperRest = rest;
  upperRest.toUpperCase();
  if (upperRest == "SWEEP") {
    sweepServo(static_cast<size_t>(index));
    return;
  }

  int angle = 0;
  if (!parseAngle(rest, angle)) {
    Serial.println(F("Angle must be a whole number."));
    return;
  }

  setServoAngle(static_cast<size_t>(index), angle, true);
}

int servoIndexFromToken(const String &token) {
  String normalized;
  normalized.reserve(token.length());
  for (size_t i = 0; i < static_cast<size_t>(token.length()); ++i) {
    char c = token.charAt(static_cast<unsigned int>(i));
    if (c == ' ' || c == '-' || c == '_') {
      continue;
    }
    normalized += static_cast<char>(toupper(static_cast<unsigned char>(c)));
  }

  if (normalized.length() == 0) {
    return -1;
  }

  bool digitsOnly = true;
  for (size_t i = 0; i < static_cast<size_t>(normalized.length()); ++i) {
    if (!isdigit(static_cast<unsigned char>(normalized.charAt(static_cast<unsigned int>(i))))) {
      digitsOnly = false;
      break;
    }
  }
  if (digitsOnly) {
    int value = normalized.toInt();
    if (value >= 0 && value < static_cast<int>(SERVO_COUNT)) {
      return value;
    }
  }

  if (normalized == "APAN" || normalized == "AP") {
    return 0;
  }
  if (normalized == "ATILT" || normalized == "AT") {
    return 1;
  }
  if (normalized == "BPAN" || normalized == "BP") {
    return 2;
  }
  if (normalized == "BTILT" || normalized == "BT") {
    return 3;
  }

  return -1;
}

bool parseAngle(const String &text, int &value) {
  if (text.length() == 0) {
    return false;
  }
  for (size_t i = 0; i < static_cast<size_t>(text.length()); ++i) {
    char c = text.charAt(static_cast<unsigned int>(i));
    if (!isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  value = text.toInt();
  return true;
}

void applyServoLimits(size_t index, int newMin, int newMax, bool verbose) {
  if (index >= SERVO_COUNT) {
    return;
  }
  int clampedMin = constrain(newMin, ABSOLUTE_MIN_DEG, ABSOLUTE_MAX_DEG);
  int clampedMax = constrain(newMax, ABSOLUTE_MIN_DEG, ABSOLUTE_MAX_DEG);
  if (clampedMin >= clampedMax) {
    Serial.println(F("Requested limits collapsed after absolute clamp."));
    return;
  }

  servoMinDeg[index] = clampedMin;
  servoMaxDeg[index] = clampedMax;
  setServoAngle(index, servoAngles[index], false);
  refreshHeadBounds();
  if (verbose) {
    Serial.printf("[%u/%s] limits set to %d-%d deg\n",
                  static_cast<unsigned>(index), SERVO_LABELS[index],
                  servoMinDeg[index], servoMaxDeg[index]);
  }
}

void resetServoLimits(size_t index, bool verbose) {
  if (index >= SERVO_COUNT) {
    return;
  }
  servoMinDeg[index] = DEFAULT_MIN_PER_SERVO[index];
  servoMaxDeg[index] = DEFAULT_MAX_PER_SERVO[index];
  setServoAngle(index, servoAngles[index], false);
  refreshHeadBounds();
  if (verbose) {
    Serial.printf("[%u/%s] limits reset to %d-%d deg\n",
                  static_cast<unsigned>(index), SERVO_LABELS[index],
                  DEFAULT_MIN_PER_SERVO[index], DEFAULT_MAX_PER_SERVO[index]);
  }
}

void resetAllLimits(bool verbose) {
  for (size_t i = 0; i < SERVO_COUNT; ++i) {
    resetServoLimits(i, false);
  }
  if (verbose) {
    Serial.println(F("All servo limits restored to defaults."));
  }
}
