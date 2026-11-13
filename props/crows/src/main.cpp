#include <Arduino.h>
#include <ESP32PWM.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>

#include "protocol.h"

// 2026 show wire format + the shared "follow the Scarecrow" rules
// (harvest-of-shadows/shared/Shared_Constants, see platformio.ini build_flags
// and harvest-of-shadows docs/show-follower.md).
#include "ProjectConstants.h"
#include "ShowFollower.h"

#ifndef EVENT_DEBUG_SHORT
#define EVENT_DEBUG_SHORT 0
#endif

// 2026-08-27: all property APs on WiFi channel 11 (NETWORK_CONSTRAINTS C1).
#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL 11
#endif

#ifndef FORCE_QUICK_AT_BOOT
#define FORCE_QUICK_AT_BOOT 0  // set 1 to force quick mode during boot
#endif

// ===================== Pin configuration =====================
static const int PIN_HEAD1 = 13;   // yaw
static const int PIN_HEAD2 = 14;   // pitch
static const int PIN_TILT1 = 12;   // tilt channel 1 (crow A). Was 26; the prop is wired like the range explorer (bench WIGGLE 2026-09-29). GPIO12 is a strapping pin: it must not be held HIGH at boot.
static const int PIN_TILT2 = 27;   // tilt channel 2

static const int PWM_FREQ_HZ = 50;
static const int SERVO_MIN_US = 500;
static const int SERVO_MAX_US = 2500;

// ===================== Angle limits =====================
static const int HEAD_MIN_ANGLE = 5;
static const int HEAD_MAX_ANGLE = 122;
static const int HEAD_NEUTRAL_ANGLE = 90;

static const int SERVO_MIN = 5;
static const int SERVO_MAX = 122;
// Per-crow tilt tops, set by bench SWEEP 2026-09-29 with Joe: crow A (GPIO12)
// goes to 130°, crow B (GPIO27) is right at 122°. Bottom is SERVO_MIN for both.
static const int TILT1_MAX = 130;
static const int TILT2_MAX = 122;
static const int TILT_NEUTRAL_ANGLE = 90;

// ===================== Event timing =====================
static const uint32_t FIRST_EVENT_MIN_MS = 30000;
static const uint32_t FIRST_EVENT_MAX_MS = 45000;
static const uint32_t EVENT_MIN_MS = 30000;
static const uint32_t EVENT_MAX_MS = 120000;
static const uint32_t REARM_MIN_MS = 30000;
static const uint32_t REARM_MAX_MS = 180000;

#if EVENT_DEBUG_SHORT
static const size_t QUEUE_LOW_WATER = 0;
static const size_t QUEUE_HIGH_WATER = 1;
#else
static const size_t QUEUE_LOW_WATER = 2;
static const size_t QUEUE_HIGH_WATER = 4;
#endif

static const uint32_t CONTROL_TICK_MS = 20;
static const float EASING_PI = 3.1415926535f;

// ===================== Motion step =====================
struct MotionStep {
  int head1;
  int head2;
  int tilt1;
  int tilt2;
  uint32_t delayMs;
  uint32_t moveMs;
  uint32_t holdMs;
};

static const size_t MAX_STEPS = 48;
static MotionStep gSteps[MAX_STEPS];
static size_t gStepCount = 0;
static size_t gStepIndex = 0;

// Motion phase
enum class Phase : uint8_t { Delay, Move, Hold, Idle };
static Phase gPhase = Phase::Idle;
static uint32_t gPhaseStartMs = 0;
static uint32_t gPhaseEndMs = 0;

// ===================== Controller state =====================
enum class ControllerState : uint8_t { Booting, IdleWaiting, EventRunning, ZoneOverride };
static ControllerState gState = ControllerState::Booting;
static Zone gZone = ZNONE;

static bool gEventRunning = false;
static bool gEventQuick = false;
static uint32_t gEventEndMs = 0;
static uint32_t gNextEventAtMs = 0;
static uint32_t gQuickUntilMs = 0;

// ===================== Servos and positions =====================
static Servo gHead1;
static Servo gHead2;
static Servo gTilt1;
static Servo gTilt2;

static float gHead1Angle = HEAD_NEUTRAL_ANGLE;
static float gHead2Angle = HEAD_NEUTRAL_ANGLE;
static float gTilt1Angle = TILT_NEUTRAL_ANGLE;
static float gTilt2Angle = TILT_NEUTRAL_ANGLE;

static float gStartHead1 = HEAD_NEUTRAL_ANGLE;
static float gStartHead2 = HEAD_NEUTRAL_ANGLE;
static float gStartTilt1 = TILT_NEUTRAL_ANGLE;
static float gStartTilt2 = TILT_NEUTRAL_ANGLE;

static float gTargetHead1 = HEAD_NEUTRAL_ANGLE;
static float gTargetHead2 = HEAD_NEUTRAL_ANGLE;
static float gTargetTilt1 = TILT_NEUTRAL_ANGLE;
static float gTargetTilt2 = TILT_NEUTRAL_ANGLE;

static float gQueuedHead1 = HEAD_NEUTRAL_ANGLE;
static float gQueuedHead2 = HEAD_NEUTRAL_ANGLE;
static float gQueuedTilt1 = TILT_NEUTRAL_ANGLE;
static float gQueuedTilt2 = TILT_NEUTRAL_ANGLE;

static bool gNextSegmentIsTilt1 = true;
#if EVENT_DEBUG_SHORT
static int gDebugSegmentsRemaining = 0;
#endif

static bool gQuickStateActive = false;
static bool gZoneActive = false;
static Zone gLastZone = ZNONE;
static uint32_t gLastHeartbeatMs = 0;

static const uint32_t HEARTBEAT_TIMEOUT_MS = 10000;
static const uint32_t QUICK_ZONE_FAR_DURATION_MS = 240000;

static char gSerialBuffer[32];
static size_t gSerialLength = 0;

// ===================== 2026 show follow =====================
// Quick mode follows the Scarecrow: on from theme pick (SHOW_NARRATOR) through
// SHOW_ACTIVE, off at SHOW_OFF (after 3:00 of show) or the follower's
// fail-safes (230 s after ACTIVE; SHOW_NARRATOR_TIMEOUT_MS if ACTIVE never comes).
static_assert(showfollow::W_CMD_START_SCENE == CMD_START_SCENE, "wire value drift");
static_assert(showfollow::W_CMD_SET_THEME == CMD_SET_THEME, "wire value drift");
static_assert(showfollow::W_CMD_SHOW_STATE == CMD_SHOW_STATE, "wire value drift");
static_assert(showfollow::W_CMD_HEARTBEAT == CMD_HEARTBEAT, "wire value drift");
static_assert(showfollow::W_NODE_SCARECROW == NODE_SCARECROW_A1S, "wire value drift");
static_assert(showfollow::W_SHOW_NARRATOR == SHOW_NARRATOR, "wire value drift");
static_assert(showfollow::W_SHOW_ACTIVE == SHOW_ACTIVE, "wire value drift");
static_assert(showfollow::W_SHOW_OFF == SHOW_OFF, "wire value drift");

static const uint32_t SHOW_NARRATOR_TIMEOUT_MS = 300000;  // pre-show that never reaches ACTIVE
static const uint32_t SHOW_QUICK_BACKSTOP_MS = 900000;    // hard cap if the follower never lets go

static showfollow::Follower makeFollower() {
  showfollow::Config c;
  c.narratorTimeoutMs = SHOW_NARRATOR_TIMEOUT_MS;
  return showfollow::Follower(c);
}
static showfollow::Follower gShow = makeFollower();
static bool gShowQuick = false;           // quick mode currently held by the show
static uint32_t gShowQuickStartMs = 0;
static uint32_t gBenchSession = 0x0B000000;

// ESP-NOW frames arrive on the WiFi task; queue them for loop().
struct ShowFrame {
  uint8_t cmd;
  uint8_t sender;  // 0 when the radio MAC isn't the Scarecrow's
  uint32_t session;
  uint32_t payloadA;
};
static const size_t SHOW_RX_SLOTS = 16;
static ShowFrame gShowRx[SHOW_RX_SLOTS];
static volatile size_t gShowRxHead = 0;
static volatile size_t gShowRxTail = 0;
static volatile uint32_t gShowRxDropped = 0;
static portMUX_TYPE gShowRxMux = portMUX_INITIALIZER_UNLOCKED;

static void quickStartWindow(uint32_t nowMs, uint32_t durationMs);
static void quickStartManual(uint32_t nowMs, uint32_t seconds);
static void quickStartZone(uint32_t nowMs, const uint8_t *mac);
static void quickExpire(uint32_t nowMs, const char *reason, bool suppressLog = false);
static void maintainQuickState(uint32_t nowMs);
static void processSerialCommand(uint32_t nowMs, char *line);
static void handleSerialInput(uint32_t nowMs);
static void initEspNow();
static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len);

// ===================== Helpers =====================
template <typename... Args>
static inline void logf(const char *fmt, Args... args) {
  Serial.printf(fmt, args...);
}

static inline int clampInt(int value, int minValue, int maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

static inline float clampFloat(float value, float minValue, float maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

static inline int randRange(int minVal, int maxVal) {
  if (maxVal <= minVal) {
    return minVal;
  }
  return minVal + static_cast<int>(random(maxVal - minVal + 1));
}

static inline uint32_t randRangeMs(uint32_t minVal, uint32_t maxVal) {
  if (maxVal <= minVal) {
    return minVal;
  }
  return minVal + static_cast<uint32_t>(random(maxVal - minVal + 1));
}

static inline bool randPercent(int percent) {
  return random(0, 100) < percent;
}

static inline bool randBool() {
  return random(0, 2) == 1;
}

static inline bool timePassed(uint32_t nowMs, uint32_t scheduledMs) {
  return static_cast<int32_t>(nowMs - scheduledMs) >= 0;
}

static bool quickProfileActive(uint32_t nowMs) {
  // Signed difference: `nowMs < gQuickUntilMs` broke at the ~49.7-day millis()
  // wrap (an expired window read as active -> stuck in quick mode).
  return static_cast<int32_t>(gQuickUntilMs - nowMs) > 0;
}

static inline int extremeBiasedTarget(int maxAngle) {
  if (randPercent(70)) {
    return randBool() ? randRange(5, 25) : randRange(maxAngle - 20, maxAngle);
  }
  return randRange(40, 87);
}

static inline void separateTargets(int &a, int &b, int minSep = 15) {
  if (abs(a - b) < minSep) {
    if (a <= b) {
      a = clampInt(a - 12, SERVO_MIN, SERVO_MAX);
    } else {
      b = clampInt(b - 12, SERVO_MIN, SERVO_MAX);
    }
  }
}

static int enforceTiltSeparation(int candidate, int other, int maxAngle, int minSep = 15) {
  if (abs(candidate - other) >= minSep) {
    return candidate;
  }
  if (candidate <= other) {
    return clampInt(other - minSep, SERVO_MIN, maxAngle);
  }
  return clampInt(other + minSep, SERVO_MIN, maxAngle);
}

static int chooseTiltTarget(int current, int other, int maxAngle) {
  int candidate = current;
  for (int attempt = 0; attempt < 6; ++attempt) {
    int trial = extremeBiasedTarget(maxAngle);
    if (abs(trial - current) < 2) {
      continue;
    }
    candidate = enforceTiltSeparation(trial, other, maxAngle);
    break;
  }
  if (candidate == current) {
    candidate = enforceTiltSeparation(current + (current < other ? -18 : 18), other, maxAngle);
  }
  return clampInt(candidate, SERVO_MIN, maxAngle);
}

static int randomHeadTarget() {
  return randRange(HEAD_MIN_ANGLE, HEAD_MAX_ANGLE);
}

static void logPlanTilt(const char *tag, int from, int to, uint32_t moveMs, uint32_t holdMs, bool skip) {
  int delta = to - from;
  if (skip) {
    logf("[%s] skip delta=%+d°\n", tag, delta);
    return;
  }
  logf("[%s] from=%d° to=%d° delta=%+d° mv=%u hd=%u\n", tag, from, to, delta, moveMs, holdMs);
}

static void logPlanHead(const char *tag, int from, int to, uint32_t moveMs, uint32_t holdMs, bool skip) {
  int delta = to - from;
  if (skip) {
    logf("[%s] skip delta=%+d°\n", tag, delta);
    return;
  }
  logf("[%s] from=%d° to=%d° delta=%+d° mv=%u hd=%u\n", tag, from, to, delta, moveMs, holdMs);
}

static int ledcTimerForChannel(int channel) {
  if (channel < 0 || channel >= NUM_PWM) {
    return -1;
  }
  ESP32PWM *pwm = ESP32PWM::ChannelUsed[channel];
  if (!pwm) {
    return -1;
  }
  return pwm->getTimer();
}

static void logLedcConfig(int pin, int channel) {
  int timer = ledcTimerForChannel(channel);
  logf("[LEDC] pin=%d ch=%d timer=%d\n", pin, channel, timer);
}

static void formatMac(const uint8_t *mac, char *out, size_t len) {
  if (!out || len == 0) {
    return;
  }
  if (!mac) {
    out[0] = '\0';
    return;
  }
  snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void quickStartWindow(uint32_t nowMs, uint32_t durationMs) {
  gQuickUntilMs = nowMs + durationMs;
  gQuickStateActive = true;
}

static void quickStartManual(uint32_t nowMs, uint32_t seconds) {
  uint32_t durationMs = seconds * 1000UL;
  quickStartWindow(nowMs, durationMs);
  gZoneActive = true;
  gLastZone = ZFAR;
  gLastHeartbeatMs = 0;
  logf("[QUICK] manual start for %lu s\n", static_cast<unsigned long>(seconds));
}

static void quickStartZone(uint32_t nowMs, const uint8_t *mac) {
  quickStartWindow(nowMs, QUICK_ZONE_FAR_DURATION_MS);
  gZoneActive = true;
  gLastZone = ZFAR;
  gLastHeartbeatMs = nowMs;
  char macStr[18] = {};
  formatMac(mac, macStr, sizeof(macStr));
  logf("[ZONE] far -> quick (src=%s) until t=%lu ms\n", macStr, static_cast<unsigned long>(gQuickUntilMs));
}

static void quickExpire(uint32_t nowMs, const char *reason, bool suppressLog) {
  bool wasActive = gQuickStateActive || quickProfileActive(nowMs);
  gQuickStateActive = false;
  gZoneActive = false;
  gQuickUntilMs = nowMs;
  gLastZone = ZNONE;
  if (wasActive && !suppressLog) {
    logf("[ZONE] quick expired -> ambient%s\n", reason ? reason : "");
  }
}

// ===================== 2026 show follow =====================
static const char *showCmdName(uint8_t cmd, uint32_t payloadA) {
  switch (cmd) {
    case CMD_SET_THEME:
      return "SET_THEME";
    case CMD_START_SCENE:
      return "START_SCENE";
    case CMD_HEARTBEAT:
      return "HEARTBEAT";
    case CMD_STOP:
      return "STOP";
    case CMD_SHOW_STATE:
      switch (payloadA) {
        case SHOW_IDLE:
          return "SHOW_IDLE";
        case SHOW_PENDING:
          return "SHOW_PENDING";
        case SHOW_NARRATOR:
          return "SHOW_NARRATOR";
        case SHOW_ACTIVE:
          return "SHOW_ACTIVE";
        case SHOW_OFF:
          return "SHOW_OFF";
        default:
          return "SHOW_STATE?";
      }
    default:
      return nullptr;  // not a show cue, not logged
  }
}

static void logShowOut(const showfollow::Out &out) {
  if (out.why) {
    logf("[show]%s %s (phase=%s)\n", out.warn ? " WARN" : "", out.why,
         showfollow::Follower::phaseName(gShow.phase()));
  }
}

// Every show frame goes through here: radio (via the rx queue) and bench serial.
static void handleShowFrame(const ShowFrame &f, uint32_t nowMs, bool fromBench) {
  if (f.cmd == CMD_STOP) {
    if (f.sender != NODE_SCARECROW_A1S) {
      logf("[show] WARN STOP from a non-Scarecrow sender ignored\n");
      return;
    }
    gShow.abort();
    logf("[show] STOP -> show forgotten%s\n", fromBench ? " (bench)" : "");
    return;
  }
  const char *name = showCmdName(f.cmd, f.payloadA);
  if (name && f.cmd != CMD_HEARTBEAT) {
    logf("[show] rx %s a=%lu sess=%08lX from=%s%s\n", name, static_cast<unsigned long>(f.payloadA),
         static_cast<unsigned long>(f.session), f.sender == NODE_SCARECROW_A1S ? "scarecrow" : "OTHER",
         fromBench ? " (bench)" : "");
  }
  logShowOut(gShow.onFrame(f.cmd, f.sender, f.session, f.payloadA, nowMs));
}

static void drainShowRx(uint32_t nowMs) {
  for (;;) {
    ShowFrame f;
    portENTER_CRITICAL(&gShowRxMux);
    if (gShowRxTail == gShowRxHead) {
      portEXIT_CRITICAL(&gShowRxMux);
      break;
    }
    f = gShowRx[gShowRxTail];
    gShowRxTail = (gShowRxTail + 1) % SHOW_RX_SLOTS;
    portEXIT_CRITICAL(&gShowRxMux);
    handleShowFrame(f, nowMs, false);
  }
  static uint32_t reportedDrops = 0;
  if (gShowRxDropped != reportedDrops) {
    reportedDrops = gShowRxDropped;
    logf("[show] WARN rx queue full, %lu frames dropped so far\n", static_cast<unsigned long>(reportedDrops));
  }
}

// Quick mode is held while the follower is in a visit or a show run.
static void maintainShowQuick(uint32_t nowMs) {
  logShowOut(gShow.tick(nowMs));
  bool want = gShow.phase() != showfollow::Phase::IDLE;

  if (want && gShowQuick && static_cast<int32_t>(nowMs - gShowQuickStartMs) >= (int32_t)SHOW_QUICK_BACKSTOP_MS) {
    gShow.abort();
    want = false;
    logf("[show] WARN quick held %lu s by the show -> backstop, back to ambient\n",
         static_cast<unsigned long>(SHOW_QUICK_BACKSTOP_MS / 1000));
  }

  if (want) {
    if (!gShowQuick) {
      gShowQuick = true;
      gShowQuickStartMs = nowMs;
      gLastHeartbeatMs = 0;  // the 2025 10 s ZONE heartbeat timeout doesn't apply
      gZoneActive = false;
      logf("[show] -> QUICK (%s)\n", showfollow::Follower::phaseName(gShow.phase()));
    }
    // Keep the window open a few seconds ahead; the follower decides when it ends.
    uint32_t until = nowMs + 5000;
    if (static_cast<int32_t>(until - gQuickUntilMs) > 0) {
      quickStartWindow(nowMs, 5000);
    }
    return;
  }

  if (gShowQuick) {
    gShowQuick = false;
    logf("[show] -> AMBIENT after %lu s quick\n", static_cast<unsigned long>((nowMs - gShowQuickStartMs) / 1000));
    quickExpire(nowMs, " (show over)");
  }
}

static void printShowStatus(uint32_t nowMs) {
  const showfollow::Stats &s = gShow.stats();
  logf("[show] phase=%s quick=%d(show=%d) runElapsed=%lus leaderSilent=%d\n",
       showfollow::Follower::phaseName(gShow.phase()), quickProfileActive(nowMs) ? 1 : 0, gShowQuick ? 1 : 0,
       static_cast<unsigned long>(gShow.runElapsed(nowMs) / 1000), gShow.leaderSilent(nowMs) ? 1 : 0);
  logf("[show] plays=%u dup=%u earlyOFF=%u deadline=%u visitTimeout=%u badSender=%u rxDropped=%lu\n", s.plays,
       s.dupIgnored, s.earlyOffIgnored, s.deadlineHits, s.visitTimeouts, s.badSender,
       static_cast<unsigned long>(gShowRxDropped));
}

static void maintainQuickState(uint32_t nowMs) {
  if (gZoneActive && gLastHeartbeatMs != 0) {
    uint32_t since = nowMs - gLastHeartbeatMs;
    if (since > HEARTBEAT_TIMEOUT_MS) {
      quickExpire(nowMs, " (hb timeout)");
      gLastHeartbeatMs = 0;
      return;
    }
  }

  if (quickProfileActive(nowMs)) {
    gQuickStateActive = true;
    return;
  }

  if (gQuickStateActive) {
    quickExpire(nowMs, nullptr);
  }
}

static void stopEvent(uint32_t nowMs, const char *reason);
static void writeServoAngles(float h1, float h2, float t1, float t2);

// Bench: swing one pin 30°<->120° `count` times (sweep=false), or glide slowly
// SERVO_MIN -> SERVO_MAX -> SERVO_MIN -> 90 `count` times (sweep=true), with the
// animation held still meanwhile. Uses the animator's own Servo for its 4 pins,
// a temporary one for any other pin.
static void wigglePin(int pin, int count, bool sweep = false, int lo = SERVO_MIN, int hi = SERVO_MAX) {
  lo = clampInt(lo, 0, 90);
  hi = clampInt(hi, 90, 180);
  static const int kServoPins[] = {2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
  bool ok = false;
  for (int p : kServoPins) {
    ok = ok || (p == pin);
  }
  if (!ok || count > 10) {
    logf("[WIGGLE] pin %d not allowed (servo pins: 2,4,5,12-19,21-23,25-27,32,33; count<=10)\n", pin);
    return;
  }
  uint32_t nowMs = millis();
  gShow.abort();
  quickExpire(nowMs, nullptr, true);
  gQuickUntilMs = 0;
  stopEvent(nowMs, "wiggle");
  writeServoAngles(HEAD_NEUTRAL_ANGLE, HEAD_NEUTRAL_ANGLE, TILT_NEUTRAL_ANGLE, TILT_NEUTRAL_ANGLE);

  Servo temp;
  Servo *s = pin == PIN_HEAD1 ? &gHead1 : pin == PIN_HEAD2 ? &gHead2 : pin == PIN_TILT1 ? &gTilt1
             : pin == PIN_TILT2 ? &gTilt2 : nullptr;
  if (!s) {
    temp.setPeriodHertz(PWM_FREQ_HZ);
    temp.attach(pin, SERVO_MIN_US, SERVO_MAX_US);
    s = &temp;
  }
  logf("[WIGGLE] GPIO%d x%d %s (%s)\n", pin, count, sweep ? "sweep" : "wiggle",
       s == &temp ? "temp servo" : "animator servo");
  for (int i = 0; i < count; ++i) {
    if (!sweep) {
      s->write(30);
      delay(700);
      s->write(120);
      delay(700);
      continue;
    }
    // 90 -> min -> max -> 90, 1° every 25 ms (~3 s end to end)
    for (int a = 90; a >= lo; --a) {
      s->write(a);
      delay(25);
    }
    logf("[WIGGLE] GPIO%d at min %d°\n", pin, lo);
    delay(500);
    for (int a = lo; a <= hi; ++a) {
      s->write(a);
      delay(25);
    }
    logf("[WIGGLE] GPIO%d at max %d°\n", pin, hi);
    delay(500);
    for (int a = hi; a >= 90; --a) {
      s->write(a);
      delay(25);
    }
  }
  s->write(90);
  delay(400);
  if (s == &temp) {
    temp.detach();
  }
  gNextEventAtMs = millis() + 60000;  // keep still a minute so the next wiggle is easy to see
  logf("[WIGGLE] GPIO%d done, animation paused 60 s\n", pin);
}

static void processSerialCommand(uint32_t nowMs, char *line) {
  if (!line) {
    return;
  }
  size_t len = strlen(line);
  while (len > 0 && isspace(static_cast<unsigned char>(line[len - 1]))) {
    line[--len] = '\0';
  }

  char *cmd = line;
  while (*cmd && isspace(static_cast<unsigned char>(*cmd))) {
    ++cmd;
  }
  for (char *p = cmd; *p; ++p) {
    if (*p == '\r' || *p == '\n') {
      *p = '\0';
      break;
    }
    *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
  }
  if (*cmd == '\0') {
    return;
  }

  if (strncmp(cmd, "QUICK", 5) == 0) {
    char *arg = cmd + 5;
    while (*arg == ' ') {
      ++arg;
    }
    unsigned long seconds = strtoul(arg, nullptr, 10);
    if (seconds == 0) {
      seconds = 240;
    }
    quickStartManual(nowMs, static_cast<uint32_t>(seconds));
  } else if (strcmp(cmd, "AMBIENT") == 0) {
    logf("[QUICK] manual cancel\n");
    quickExpire(nowMs, " (manual cancel)", true);
    gQuickUntilMs = 0;
    gLastHeartbeatMs = 0;
  }
  // Bench show cues: same path as radio frames (handleShowFrame), acting as the Scarecrow.
  // VISIT = new session SET_THEME + SHOW_NARRATOR. FAKE = SHOW_NARRATOR from a non-Scarecrow sender.
  else if (strcmp(cmd, "VISIT") == 0) {
    ++gBenchSession;
    handleShowFrame({CMD_SET_THEME, NODE_SCARECROW_A1S, gBenchSession, THEME_CORN}, nowMs, true);
    handleShowFrame({CMD_SHOW_STATE, NODE_SCARECROW_A1S, gBenchSession, SHOW_NARRATOR}, nowMs, true);
  } else if (strcmp(cmd, "ACTIVE") == 0) {
    handleShowFrame({CMD_START_SCENE, NODE_SCARECROW_A1S, gBenchSession, THEME_CORN}, nowMs, true);
    handleShowFrame({CMD_SHOW_STATE, NODE_SCARECROW_A1S, gBenchSession, SHOW_ACTIVE}, nowMs, true);
  } else if (strcmp(cmd, "OFF") == 0) {
    handleShowFrame({CMD_SHOW_STATE, NODE_SCARECROW_A1S, gBenchSession, SHOW_OFF}, nowMs, true);
  } else if (strcmp(cmd, "IDLE") == 0) {
    handleShowFrame({CMD_SHOW_STATE, NODE_SCARECROW_A1S, gBenchSession, SHOW_IDLE}, nowMs, true);
  } else if (strcmp(cmd, "STOP") == 0) {
    handleShowFrame({CMD_STOP, NODE_SCARECROW_A1S, gBenchSession, 0}, nowMs, true);
  } else if (strcmp(cmd, "FAKE") == 0) {
    handleShowFrame({CMD_SHOW_STATE, NODE_MAIN_SWITCH, gBenchSession + 1, SHOW_NARRATOR}, nowMs, true);
  } else if (strcmp(cmd, "SHOW") == 0) {
    printShowStatus(nowMs);
  } else if (strncmp(cmd, "WIGGLE", 6) == 0) {
    // WIGGLE <pin> [count]: find which servo is on which pin (2026-09-29, tilts not moving).
    char *end = nullptr;
    long pin = strtol(cmd + 6, &end, 10);
    long count = end ? strtol(end, nullptr, 10) : 0;
    wigglePin(static_cast<int>(pin), count > 0 ? static_cast<int>(count) : 2);
  } else if (strncmp(cmd, "SWEEP", 5) == 0) {
    // SWEEP <pin> [count] [lo] [hi]: slow glide, default 5°..122° (the animator's limits), 0..180 max.
    char *end = nullptr;
    long pin = strtol(cmd + 5, &end, 10);
    long count = end ? strtol(end, &end, 10) : 0;
    long lo = end ? strtol(end, &end, 10) : 0;   // optional range, 0 = animator limit
    long hi = end ? strtol(end, &end, 10) : 0;
    wigglePin(static_cast<int>(pin), count > 0 ? static_cast<int>(count) : 1, true,
              lo > 0 ? static_cast<int>(lo) : SERVO_MIN, hi > 0 ? static_cast<int>(hi) : SERVO_MAX);
  } else {
    logf("[CMD] unknown '%s' (QUICK [s], AMBIENT, VISIT, ACTIVE, OFF, IDLE, STOP, FAKE, SHOW, WIGGLE pin [n], SWEEP pin [n] [lo] [hi])\n",
         cmd);
  }
}

static void handleSerialInput(uint32_t nowMs) {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      gSerialBuffer[gSerialLength] = '\0';
      processSerialCommand(nowMs, gSerialBuffer);
      gSerialLength = 0;
      gSerialBuffer[0] = '\0';
      continue;
    }
    if (gSerialLength + 1 < sizeof(gSerialBuffer)) {
      gSerialBuffer[gSerialLength++] = c;
      gSerialBuffer[gSerialLength] = '\0';
    }
  }
}

static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len <= 0 || data == nullptr) {
    return;
  }
  uint32_t nowMs = millis();

  // 2026 show Msg: must pass magic/version/CRC; the sender only counts as the
  // Scarecrow when both the radio MAC and the Msg sender field say so.
  if (len == static_cast<int>(sizeof(Msg))) {
    Msg m;
    memcpy(&m, data, sizeof(m));
    if (!msg_valid(m)) {
      return;
    }
    bool fromScarecrow = mac && memcmp(mac, MAC_SCARECROW_A1S, 6) == 0 && m.sender == NODE_SCARECROW_A1S;
    ShowFrame f{m.command, static_cast<uint8_t>(fromScarecrow ? NODE_SCARECROW_A1S : 0), m.session_id,
                m.payload_a};
    portENTER_CRITICAL_ISR(&gShowRxMux);
    size_t next = (gShowRxHead + 1) % SHOW_RX_SLOTS;
    if (next == gShowRxTail) {
      gShowRxDropped = gShowRxDropped + 1;
    } else {
      gShowRx[gShowRxHead] = f;
      gShowRxHead = next;
    }
    portEXIT_CRITICAL_ISR(&gShowRxMux);
    return;
  }

  char token[16];
  size_t copyLen = static_cast<size_t>(len);
  if (copyLen >= sizeof(token)) {
    copyLen = sizeof(token) - 1;
  }
  memcpy(token, data, copyLen);
  token[copyLen] = '\0';
  for (size_t i = 0; token[i]; ++i) {
    if (token[i] == '\r' || token[i] == '\n') {
      token[i] = '\0';
      break;
    }
    token[i] = static_cast<char>(toupper(static_cast<unsigned char>(token[i])));
  }

  if (strcmp(token, "ZONE_FAR") == 0) {
    quickStartZone(nowMs, mac);
  } else if (strcmp(token, "HEARTBEAT") == 0) {
    gLastHeartbeatMs = nowMs;
    logf("[HB] rx @ %lu ms\n", static_cast<unsigned long>(nowMs));
  }
}

static void initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    logf("[ESP-NOW] init failed\n");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  logf("[ESP-NOW] ready ch=%d\n", ESPNOW_CHANNEL);
}

// ===================== State helpers =====================
static const char *stateName(ControllerState state) {
  switch (state) {
    case ControllerState::Booting:
      return "BOOTING";
    case ControllerState::IdleWaiting:
      return "IDLE_WAIT";
    case ControllerState::EventRunning:
      return gEventQuick ? "ACTION" : "AMBIENT";
    case ControllerState::ZoneOverride:
      return "ZONE_OVERRIDE";
    default:
      return "?";
  }
}

static void setState(ControllerState next) {
  if (gState == next) {
    return;
  }
  gState = next;
  logf("[STATE] -> %s @ %lu ms\n", stateName(gState), millis());
}

// ===================== Queue helpers =====================
static void resetQueue() {
  gStepCount = 0;
  gStepIndex = 0;
  gPhase = Phase::Idle;
  gPhaseStartMs = gPhaseEndMs = 0;
}

static inline size_t queueDepth() {
  size_t pending = gStepCount - gStepIndex;
  if (gPhase != Phase::Idle && gStepIndex < gStepCount && pending > 0) {
    --pending;
  }
  return pending;
}

static void syncPlanningState() {
  gQueuedHead1 = gHead1Angle;
  gQueuedHead2 = gHead2Angle;
  gQueuedTilt1 = gTilt1Angle;
  gQueuedTilt2 = gTilt2Angle;
}

static bool planNextSegment(bool quick);
static void refillQueue(bool quick);

static bool enqueueStep(int head1, int head2, int tilt1, int tilt2, uint32_t delayMs, uint32_t moveMs,
                        uint32_t holdMs) {
  if (gStepCount >= MAX_STEPS) {
    return false;
  }
  if (queueDepth() >= QUEUE_HIGH_WATER) {
    return false;
  }
  gSteps[gStepCount++] = {head1, head2, tilt1, tilt2, delayMs, moveMs, holdMs};
  return true;
}

static void writeServoAngles(float h1, float h2, float t1, float t2) {
  gHead1Angle = clampFloat(h1, HEAD_MIN_ANGLE, HEAD_MAX_ANGLE);
  gHead2Angle = clampFloat(h2, HEAD_MIN_ANGLE, HEAD_MAX_ANGLE);
  gTilt1Angle = clampFloat(t1, SERVO_MIN, TILT1_MAX);
  gTilt2Angle = clampFloat(t2, SERVO_MIN, TILT2_MAX);

  gHead1.write(static_cast<int>(lroundf(gHead1Angle)));
  gHead2.write(static_cast<int>(lroundf(gHead2Angle)));
  gTilt1.write(static_cast<int>(lroundf(gTilt1Angle)));
  gTilt2.write(static_cast<int>(lroundf(gTilt2Angle)));
}

static void logAxisStart(const char *tag, float from, float to, uint32_t moveMs, uint32_t holdMs,
                         size_t depth, uint32_t nowMs) {
  if (fabsf(to - from) < 0.5f) {
    return;
  }
  int fromInt = static_cast<int>(lroundf(from));
  int toInt = static_cast<int>(lroundf(to));
  logf("[%s] start from=%d° to=%d° mv=%u hd=%u qdepth=%u @ %lu ms\n", tag, fromInt, toInt, moveMs, holdMs,
       static_cast<unsigned>(depth), static_cast<unsigned long>(nowMs));
}

static void logSegmentStart(const MotionStep &step, uint32_t nowMs) {
  size_t depth = queueDepth();
  logAxisStart("TILT1", gStartTilt1, gTargetTilt1, step.moveMs, step.holdMs, depth, nowMs);
  logAxisStart("TILT2", gStartTilt2, gTargetTilt2, step.moveMs, step.holdMs, depth, nowMs);
  logAxisStart("HEAD1", gStartHead1, gTargetHead1, step.moveMs, step.holdMs, depth, nowMs);
  logAxisStart("HEAD2", gStartHead2, gTargetHead2, step.moveMs, step.holdMs, depth, nowMs);
}

static void beginStep(uint32_t nowMs) {
  if (gStepIndex >= gStepCount) {
    gPhase = Phase::Idle;
    return;
  }

  const MotionStep &step = gSteps[gStepIndex];

  gStartHead1 = gHead1Angle;
  gStartHead2 = gHead2Angle;
  gStartTilt1 = gTilt1Angle;
  gStartTilt2 = gTilt2Angle;

  gTargetHead1 = static_cast<float>(step.head1);
  gTargetHead2 = static_cast<float>(step.head2);
  gTargetTilt1 = static_cast<float>(step.tilt1);
  gTargetTilt2 = static_cast<float>(step.tilt2);

  if (step.delayMs > 0) {
    gPhase = Phase::Delay;
    gPhaseStartMs = nowMs;
    gPhaseEndMs = nowMs + step.delayMs;
    return;
  }

  gPhase = Phase::Move;
  gPhaseStartMs = nowMs;
  gPhaseEndMs = nowMs + step.moveMs;
  logSegmentStart(step, nowMs);
  if (step.moveMs == 0) {
    writeServoAngles(gTargetHead1, gTargetHead2, gTargetTilt1, gTargetTilt2);
  }
}

static void advancePhase(Phase nextPhase, uint32_t nowMs, uint32_t durationMs) {
  gPhase = nextPhase;
  gPhaseStartMs = nowMs;
  gPhaseEndMs = nowMs + durationMs;
  if (nextPhase == Phase::Move) {
    const MotionStep &step = gSteps[gStepIndex];
    logSegmentStart(step, nowMs);
  }
}

static void updateMotion(uint32_t nowMs) {
  if (gPhase == Phase::Idle || gStepIndex >= gStepCount) {
    return;
  }

  const MotionStep &step = gSteps[gStepIndex];

  if (gPhase == Phase::Delay) {
    if (timePassed(nowMs, gPhaseEndMs)) {
      advancePhase(Phase::Move, nowMs, step.moveMs);
      if (step.moveMs == 0) {
        writeServoAngles(gTargetHead1, gTargetHead2, gTargetTilt1, gTargetTilt2);
      }
    }
    return;
  }

  if (gPhase == Phase::Move) {
    if (step.moveMs == 0 || timePassed(nowMs, gPhaseEndMs)) {
      writeServoAngles(gTargetHead1, gTargetHead2, gTargetTilt1, gTargetTilt2);
      advancePhase(Phase::Hold, nowMs, step.holdMs);
      return;
    }

    float span = static_cast<float>(step.moveMs);
    float progress = clampFloat(static_cast<float>(nowMs - gPhaseStartMs) / span, 0.0f, 1.0f);
    float eased = 0.5f - 0.5f * cosf(progress * EASING_PI);

    float head1 = gStartHead1 + (gTargetHead1 - gStartHead1) * eased;
    float head2 = gStartHead2 + (gTargetHead2 - gStartHead2) * eased;
    float tilt1 = gStartTilt1 + (gTargetTilt1 - gStartTilt1) * eased;
    float tilt2 = gStartTilt2 + (gTargetTilt2 - gStartTilt2) * eased;

    writeServoAngles(head1, head2, tilt1, tilt2);
    return;
  }

  if (gPhase == Phase::Hold) {
    if (timePassed(nowMs, gPhaseEndMs)) {
      gPhase = Phase::Idle;
      ++gStepIndex;
      if (gEventRunning && queueDepth() <= QUEUE_LOW_WATER && !timePassed(nowMs, gEventEndMs)) {
        bool quick = quickProfileActive(nowMs);
        refillQueue(quick);
      }
      beginStep(nowMs);
    }
  }
}

// ===================== Event planning =====================
struct Band { uint16_t moveMin, moveMax, holdMin, holdMax; };
static const Band BAND_AMBIENT = {600, 1600, 800, 3000};
static const Band BAND_QUICK = {300, 900, 200, 900};

static bool planNextSegment(bool quick) {
  if (queueDepth() >= QUEUE_HIGH_WATER) {
    return false;
  }
#if EVENT_DEBUG_SHORT
  if (gDebugSegmentsRemaining == 0) {
    return false;
  }
#endif

  const Band &band = quick ? BAND_QUICK : BAND_AMBIENT;
  uint32_t moveMs = randRange(band.moveMin, band.moveMax);
  uint32_t holdMs = randRange(band.holdMin, band.holdMax);

  int prevTilt1 = static_cast<int>(lroundf(gQueuedTilt1));
  int prevTilt2 = static_cast<int>(lroundf(gQueuedTilt2));
  int prevHead1 = static_cast<int>(lroundf(gQueuedHead1));
  int prevHead2 = static_cast<int>(lroundf(gQueuedHead2));

  int nextTilt1 = prevTilt1;
  int nextTilt2 = prevTilt2;
  const bool planTilt1 = gNextSegmentIsTilt1;
  if (planTilt1) {
    nextTilt1 = chooseTiltTarget(prevTilt1, prevTilt2, TILT1_MAX);
  } else {
    nextTilt2 = chooseTiltTarget(prevTilt2, prevTilt1, TILT2_MAX);
  }

  bool tilt1Skip = (nextTilt1 == prevTilt1);
  bool tilt2Skip = (nextTilt2 == prevTilt2);

  int desiredHead1 = randomHeadTarget();
  int desiredHead2 = randomHeadTarget();
  separateTargets(desiredHead1, desiredHead2, 10);

  logPlanTilt("PLAN_TILT1", prevTilt1, nextTilt1, moveMs, holdMs, tilt1Skip);
  logPlanTilt("PLAN_TILT2", prevTilt2, nextTilt2, moveMs, holdMs, tilt2Skip);

  bool head1Skip = abs(desiredHead1 - prevHead1) < 2;
  bool head2Skip = abs(desiredHead2 - prevHead2) < 2;
  int nextHead1 = head1Skip ? prevHead1 : desiredHead1;
  int nextHead2 = head2Skip ? prevHead2 : desiredHead2;

  logPlanHead("PLAN_HEAD1", prevHead1, nextHead1, moveMs, holdMs, head1Skip);
  logPlanHead("PLAN_HEAD2", prevHead2, nextHead2, moveMs, holdMs, head2Skip);

  if (!enqueueStep(nextHead1, nextHead2, nextTilt1, nextTilt2, 0, moveMs, holdMs)) {
    return false;
  }

  gQueuedHead1 = static_cast<float>(nextHead1);
  gQueuedHead2 = static_cast<float>(nextHead2);
  gQueuedTilt1 = static_cast<float>(nextTilt1);
  gQueuedTilt2 = static_cast<float>(nextTilt2);
  gNextSegmentIsTilt1 = !gNextSegmentIsTilt1;

#if EVENT_DEBUG_SHORT
  if (gDebugSegmentsRemaining > 0) {
    --gDebugSegmentsRemaining;
  }
#endif
  return true;
}

static void refillQueue(bool quick) {
  while (queueDepth() < QUEUE_HIGH_WATER) {
    if (!planNextSegment(quick)) {
      break;
    }
  }
}

static void planEvent(bool quick) {
  resetQueue();
  syncPlanningState();
  gNextSegmentIsTilt1 = randBool();
#if EVENT_DEBUG_SHORT
  gDebugSegmentsRemaining = 1;
#endif
  refillQueue(quick);
}

// ===================== Event lifecycle =====================
static void startEvent(uint32_t nowMs, bool forceQuick) {
  if (gEventRunning) {
    return;
  }

  gEventQuick = forceQuick || quickProfileActive(nowMs);
#if EVENT_DEBUG_SHORT
  uint32_t duration = 6000;
#else
  uint32_t duration = randRangeMs(EVENT_MIN_MS, EVENT_MAX_MS);
#endif
  gEventEndMs = nowMs + duration;

  planEvent(gEventQuick);
  gStepIndex = 0;
  beginStep(nowMs);

  gEventRunning = true;
  setState(ControllerState::EventRunning);
  logf("[EVENT] start duration=%.1fs quick=%d\n", duration / 1000.0f, gEventQuick ? 1 : 0);
}

static void stopEvent(uint32_t nowMs, const char *reason) {
  if (!gEventRunning) {
    return;
  }
  gEventRunning = false;
  resetQueue();
  writeServoAngles(HEAD_NEUTRAL_ANGLE, HEAD_NEUTRAL_ANGLE, TILT_NEUTRAL_ANGLE, TILT_NEUTRAL_ANGLE);
  syncPlanningState();

  uint32_t delay = randRangeMs(REARM_MIN_MS, REARM_MAX_MS);
  gNextEventAtMs = nowMs + delay;
  logf("[EVENT] stop reason=%s\n", reason);
  logf("[SCHED] next_event_in=%u ms\n", delay);
  setState(ControllerState::IdleWaiting);
}

static void updateEvent(uint32_t nowMs) {
  if (!gEventRunning) {
    return;
  }

  updateMotion(nowMs);

  if (gPhase == Phase::Idle && gStepIndex >= gStepCount && !timePassed(nowMs, gEventEndMs)) {
    bool quick = quickProfileActive(nowMs);
    refillQueue(quick);
    if (gStepIndex < gStepCount) {
      beginStep(nowMs);
    }
  }

  if (timePassed(nowMs, gEventEndMs)) {
    stopEvent(nowMs, "duration-complete");
  }
}

// ===================== State machine =====================
static void tickController(uint32_t nowMs) {
  bool quickActive = quickProfileActive(nowMs) || gQuickStateActive;
  if (quickActive && !gEventRunning) {
    startEvent(nowMs, true);
  }

  bool zoneActive = (gZone != ZNONE);

  if (zoneActive && !gEventRunning) {
    startEvent(nowMs, true);
  }

  if (gEventRunning) {
    updateEvent(nowMs);
    return;
  }

  if (timePassed(nowMs, gNextEventAtMs)) {
    startEvent(nowMs, false);
    return;
  }

  setState(ControllerState::IdleWaiting);
}

// ===================== Zone hook =====================
void setZoneState(Zone newZone) {
  if (newZone == gZone) {
    return;
  }
  Zone old = gZone;
  gZone = newZone;
  gLastZone = newZone;
  gZoneActive = (newZone != ZNONE);
  logf("[ZONE] -> %s @ %lu ms\n", zname(gZone), millis());
  if (newZone == ZFAR) {
    uint32_t nowMs = millis();
    quickStartWindow(nowMs, QUICK_ZONE_FAR_DURATION_MS);
    logf("[ZONE] FAR start 4min\n");
    gLastHeartbeatMs = nowMs;
  } else if (old == ZFAR && newZone != ZFAR) {
    quickExpire(millis(), " (zone change)");
    logf("[ZONE] FAR end\n");
  }
}

// ===================== Arduino lifecycle =====================
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.printf("[BOOT] reset_reason=%d\n", static_cast<int>(esp_reset_reason()));
  Serial.println(F("[BOOT] Crow animator starting v2026-09-29-show2"));
  Serial.flush();

  randomSeed(esp_random());

  initEspNow();

  gHead1.setPeriodHertz(PWM_FREQ_HZ);
  int chHead1 = gHead1.attach(PIN_HEAD1, SERVO_MIN_US, SERVO_MAX_US);
  gHead2.setPeriodHertz(PWM_FREQ_HZ);
  int chHead2 = gHead2.attach(PIN_HEAD2, SERVO_MIN_US, SERVO_MAX_US);
  gTilt1.setPeriodHertz(PWM_FREQ_HZ);
  int chTilt1 = gTilt1.attach(PIN_TILT1, SERVO_MIN_US, SERVO_MAX_US);
  gTilt2.setPeriodHertz(PWM_FREQ_HZ);
  int chTilt2 = gTilt2.attach(PIN_TILT2, SERVO_MIN_US, SERVO_MAX_US);

  writeServoAngles(HEAD_NEUTRAL_ANGLE, HEAD_NEUTRAL_ANGLE, TILT_NEUTRAL_ANGLE, TILT_NEUTRAL_ANGLE);
  syncPlanningState();

  logLedcConfig(PIN_HEAD1, chHead1);
  logLedcConfig(PIN_HEAD2, chHead2);
  logLedcConfig(PIN_TILT1, chTilt1);
  logLedcConfig(PIN_TILT2, chTilt2);

  writeServoAngles(60.0f, 60.0f, TILT_NEUTRAL_ANGLE, TILT_NEUTRAL_ANGLE);
  delay(200);
  writeServoAngles(120.0f, 120.0f, TILT_NEUTRAL_ANGLE, TILT_NEUTRAL_ANGLE);
  delay(200);
  writeServoAngles(HEAD_NEUTRAL_ANGLE, HEAD_NEUTRAL_ANGLE, TILT_NEUTRAL_ANGLE, TILT_NEUTRAL_ANGLE);
  delay(150);
  syncPlanningState();

  logf("[PINS] head1=%d head2=%d tilt1=%d tilt2=%d\n", PIN_HEAD1, PIN_HEAD2, PIN_TILT1, PIN_TILT2);

  uint32_t nowMs = millis();
  gNextEventAtMs = nowMs + randRangeMs(FIRST_EVENT_MIN_MS, FIRST_EVENT_MAX_MS);
  logf("[BOOT] first event in %u ms\n", gNextEventAtMs - nowMs);

#if FORCE_QUICK_AT_BOOT
  quickStartWindow(nowMs, QUICK_ZONE_FAR_DURATION_MS);
  gZoneActive = true;
  gLastZone = ZFAR;
  logf("[QUICK] forced at boot for 240 s\n");
#endif

  setState(ControllerState::IdleWaiting);
}

void loop() {
  uint32_t nowMs = millis();
  handleSerialInput(nowMs);
  drainShowRx(nowMs);
  maintainShowQuick(nowMs);
  maintainQuickState(nowMs);
  tickController(nowMs);
  delay(CONTROL_TICK_MS);
}
