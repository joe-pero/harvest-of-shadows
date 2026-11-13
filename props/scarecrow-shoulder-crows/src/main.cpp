// =============================================================================
// Scarecrow shoulder crows (2026-09-29)
// =============================================================================
// Two crows on the Scarecrow's shoulders: one turns its head, one tilts. An
// AITRIP MP3/amp module plays ONE fixed crow track (crows_loop.mp3, the only
// file on its card); a JZ-MOS on PIN_MP3 switches its 5 V. Power-on = the
// track starts from the top, so the call times in crow_track.h (+ the player's
// boot delay, tuned on the bench with DELAY) tell us when each caw is heard.
//
// Behaviour (Joe): the sound is the same in every phase; the movement changes.
//   idle        slow random moves in events (like props/crows), player ON, looping
//   narration   (Scarecrow SHOW_NARRATOR) player OFF so the voices are clear,
//               slow moves continue
//   main show   (SHOW_ACTIVE) fast moves, player ON; restarted early when the
//               next call is far away, so the show gets a few more caws
//   every call  both crows do a quick "caw" gesture timed to the sound
// The player is power-cycled at the end of each 5:00 loop: a known restart,
// so the timing never drifts.
//
// Show cues come from shared/Shared_Constants/ShowFollower.h, the same rules
// as props/crows, Spooky Tree and Pumpkin Men (docs/show-follower.md).
// Bench commands: see printHelp().
// =============================================================================
#include <Arduino.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <math.h>

#include "ProjectConstants.h"
#include "ShowFollower.h"
#include "crow_track.h"

#define FW_VERSION "v2026-10-01-shoulder13"

#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL 11  // NETWORK_CONSTRAINTS C1
#endif

// ===================== Pins =====================
// The two servos can be plugged into 13/14 either way round: `SWAP` flips which
// one is the head crow and saves it (NVS). Avoid strapping pins 0/2/5/12/15.
static const int PIN_SERVO_A = 13;
static const int PIN_SERVO_B = 14;
static const int PIN_MP3 = 25;  // JZ-MOS data. 10 kΩ to GND keeps the player off during boot.
static const int PIN_EYES = 26;  // 2nd JZ-MOS data -> LED eyes (PWM fade). 10 kΩ to GND.

// ===================== Ranges (head set by SWEEP 2026-10-01; tilt still a placeholder) =====================
// Head: SG90, SWEEP with Joe 2026-10-01 (bare servo, head not mounted yet). Uneven on purpose
// (Joe: wider on the 175 side); centre stays 90 = facing forward, not the range midpoint.
static const int HEAD_MIN = 30, HEAD_MAX = 140, HEAD_HOME = 90;  // Joe jog 10-01: binds past 140; low end 30 (Joe, was 25)
static const int TILT_MIN = 30, TILT_MAX = 150, TILT_HOME = 90;

// ===================== Timing =====================
static const uint32_t CONTROL_TICK_MS = 20;
static const uint32_t PLAYER_OFF_GAP_MS = 1500;       // power off this long for a clean restart
static const uint32_t DEFAULT_BOOT_DELAY_MS = 800;    // player power-on -> audio; tune with DELAY
// Idle sound (Joe 10-01): a short burst of the first 3-4 calls, then the player goes off and the
// gaps grow (30 s x1.5 each time, max 4 min, +/-20 %), "slower to even slower". Reset by a show.
static const uint8_t IDLE_BURST_CALLS_MIN = 3, IDLE_BURST_CALLS_MAX = 4;
static const uint32_t IDLE_GAP_FIRST_MS = 30000, IDLE_GAP_MAX_MS = 240000;
static const float IDLE_GAP_GROWTH = 1.5f;
static const uint8_t IDLE_SLEEPY_STEPS = 5;  // movement is fully "drowsy" after this many bursts
static const uint32_t CALL_STALE_MS = 500;            // a call found later than this is skipped, not gestured

static const uint32_t SHOW_NARRATOR_TIMEOUT_MS = 300000;
static const uint32_t SHOW_QUICK_BACKSTOP_MS = 900000;

// Bird-like movement (2026-10-01, Joe: "make the loop look real"): a crow's head moves in quick
// snaps (fast turn, dead stop, hold), mostly small looks near forward, now and then a big look
// over the shoulder, a double-take or a twitch, and sometimes a still rest facing forward.
// Weights are relative (any total). Holds/rests in ms. speed scales the snap time (>1 = slower).
struct Mood {
  const char *name;
  uint8_t wSmall, wBig, wDouble, wTwitch, wRest;
  uint16_t holdMin, holdMax, restMin, restMax;
  float speed;
  uint8_t tiltCockPct;  // chance the tilt crow cocks its head right after a head look
};
static const Mood MOOD_IDLE = {"IDLE", 45, 10, 7, 28, 10, 1000, 4000, 5000, 20000, 1.0f, 35};
static const Mood MOOD_NARRATION = {"NARRATION", 35, 5, 3, 40, 17, 1500, 5000, 6000, 20000, 1.3f, 25};
static const Mood MOOD_SHOW = {"SHOW", 40, 25, 12, 20, 3, 300, 1500, 1500, 4000, 0.85f, 50};
// Snap time for a turn of d degrees: SG90 ~0.1 s / 60 deg at 5 V -> ~1.7 ms/deg; a little slower
// than flat out so it stops cleanly instead of overshooting.
static inline uint32_t snapMs(float d, float speed) { return static_cast<uint32_t>((70 + fabsf(d) * 2.2f) * speed); }

// ===================== Helpers =====================
template <typename... Args>
static inline void logf(const char *fmt, Args... args) {
  Serial.printf(fmt, args...);
}
static inline bool reached(uint32_t now, uint32_t t) { return static_cast<int32_t>(now - t) >= 0; }
static inline int32_t since(uint32_t now, uint32_t t) { return static_cast<int32_t>(now - t); }
static inline uint32_t randMs(uint32_t lo, uint32_t hi) { return hi <= lo ? lo : lo + random(hi - lo + 1); }
static inline int randInt(int lo, int hi) { return hi <= lo ? lo : lo + static_cast<int>(random(hi - lo + 1)); }

// ===================== Axes =====================
struct Axis {
  explicit Axis(const char *n) : name(n) {}
  const char *name;
  Servo servo;
  int pin = -1;
  int minA = 0, maxA = 180, homeA = 90;
  float angle = 90, from = 90, to = 90;
  uint32_t moveStart = 0, moveMs = 0, holdUntil = 0;
  bool moving = false;
  bool snap = false;               // ease-out (fast start, soft stop) instead of the smooth S-curve
  int queued = -1;                 // a follow-up move (double-take), run when this one's hold ends
  uint32_t qMoveMs = 0, qHoldMs = 0;
  int home() const { return homeA; }
};
static Axis gHead("HEAD");
static Axis gTilt("TILT");
static bool gSwapped = false;  // false: head = PIN_SERVO_A, tilt = PIN_SERVO_B

static void axisWrite(Axis &a, float deg) {
  a.angle = constrain(deg, static_cast<float>(a.minA), static_cast<float>(a.maxA));
  a.servo.write(static_cast<int>(lroundf(a.angle)));
}

static void axisMove(Axis &a, int target, uint32_t moveMs, uint32_t holdMs, uint32_t now, bool snap = false) {
  a.snap = snap;
  a.queued = -1;
  a.from = a.angle;
  a.to = constrain(target, a.minA, a.maxA);
  a.moveStart = now;
  a.moveMs = moveMs;
  a.holdUntil = now + moveMs + holdMs;
  a.moving = true;
}

static void axisUpdate(Axis &a, uint32_t now) {
  if (!a.moving) return;
  int32_t el = since(now, a.moveStart);
  if (el >= static_cast<int32_t>(a.moveMs)) {
    axisWrite(a, a.to);
    a.moving = false;
    return;
  }
  float p = el <= 0 ? 0.0f : static_cast<float>(el) / a.moveMs;
  float q = 1.0f - p;
  float eased = a.snap ? 1.0f - q * q * q : 0.5f - 0.5f * cosf(p * 3.1415926535f);
  axisWrite(a, a.from + (a.to - a.from) * eased);
}

static bool axisIdle(const Axis &a, uint32_t now) { return !a.moving && reached(now, a.holdUntil); }

static void attachAxes() {
  gHead.pin = gSwapped ? PIN_SERVO_B : PIN_SERVO_A;
  gTilt.pin = gSwapped ? PIN_SERVO_A : PIN_SERVO_B;
  gHead.minA = HEAD_MIN; gHead.maxA = HEAD_MAX; gHead.homeA = HEAD_HOME;
  gTilt.minA = TILT_MIN; gTilt.maxA = TILT_MAX; gTilt.homeA = TILT_HOME;
  for (Axis *a : {&gHead, &gTilt}) {
    a->servo.setPeriodHertz(50);
    a->servo.attach(a->pin, 500, 2500);
    a->angle = a->from = a->to = a->home();
    axisWrite(*a, a->home());
    a->moving = false;
  }
  logf("[PINS] head=GPIO%d (%d-%d) tilt=GPIO%d (%d-%d) mp3=GPIO%d swapped=%d\n", gHead.pin, gHead.minA,
       gHead.maxA, gTilt.pin, gTilt.minA, gTilt.maxA, PIN_MP3, gSwapped ? 1 : 0);
}

// ===================== Settings (NVS) =====================
static Preferences gPrefs;
static uint32_t gBootDelayMs = DEFAULT_BOOT_DELAY_MS;

// ===================== Show follow =====================
static_assert(showfollow::W_CMD_START_SCENE == CMD_START_SCENE, "wire value drift");
static_assert(showfollow::W_CMD_SET_THEME == CMD_SET_THEME, "wire value drift");
static_assert(showfollow::W_CMD_SHOW_STATE == CMD_SHOW_STATE, "wire value drift");
static_assert(showfollow::W_CMD_HEARTBEAT == CMD_HEARTBEAT, "wire value drift");
static_assert(showfollow::W_NODE_SCARECROW == NODE_SCARECROW_A1S, "wire value drift");
static_assert(showfollow::W_SHOW_NARRATOR == SHOW_NARRATOR, "wire value drift");
static_assert(showfollow::W_SHOW_ACTIVE == SHOW_ACTIVE, "wire value drift");
static_assert(showfollow::W_SHOW_OFF == SHOW_OFF, "wire value drift");

static showfollow::Follower makeFollower() {
  showfollow::Config c;
  c.narratorTimeoutMs = SHOW_NARRATOR_TIMEOUT_MS;
  return showfollow::Follower(c);
}
static showfollow::Follower gShow = makeFollower();
static uint32_t gBenchSession = 0x0C000000;
static showfollow::Phase gLastPhase = showfollow::Phase::IDLE;
static uint32_t gPhaseSinceMs = 0;

struct ShowFrame {
  uint8_t cmd;
  uint8_t sender;  // NODE_SCARECROW_A1S only when MAC + sender field both match
  uint32_t session;
  uint32_t payloadA;
};
static const size_t SHOW_RX_SLOTS = 16;
static ShowFrame gShowRx[SHOW_RX_SLOTS];
static volatile size_t gShowRxHead = 0, gShowRxTail = 0;
static volatile uint32_t gShowRxDropped = 0;
static portMUX_TYPE gShowRxMux = portMUX_INITIALIZER_UNLOCKED;

static const char *phaseName(showfollow::Phase p) { return showfollow::Follower::phaseName(p); }

static void logShowOut(const showfollow::Out &out) {
  if (out.why) logf("[show]%s %s (phase=%s)\n", out.warn ? " WARN" : "", out.why, phaseName(gShow.phase()));
}

static void handleShowFrame(const ShowFrame &f, uint32_t now, bool fromBench) {
  if (f.cmd == CMD_STOP) {
    if (f.sender != NODE_SCARECROW_A1S) {
      logf("[show] WARN STOP from a non-Scarecrow sender ignored\n");
      return;
    }
    gShow.abort();
    logf("[show] STOP -> show forgotten%s\n", fromBench ? " (bench)" : "");
    return;
  }
  if (f.cmd != CMD_HEARTBEAT &&
      (f.cmd == CMD_SET_THEME || f.cmd == CMD_START_SCENE || f.cmd == CMD_SHOW_STATE)) {
    logf("[show] rx cmd=0x%02X a=%lu sess=%08lX from=%s%s\n", f.cmd, static_cast<unsigned long>(f.payloadA),
         static_cast<unsigned long>(f.session), f.sender == NODE_SCARECROW_A1S ? "scarecrow" : "OTHER",
         fromBench ? " (bench)" : "");
  }
  logShowOut(gShow.onFrame(f.cmd, f.sender, f.session, f.payloadA, now));
}

static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != static_cast<int>(sizeof(Msg)) || !data) return;
  Msg m;
  memcpy(&m, data, sizeof(m));
  if (!msg_valid(m)) return;
  bool fromScarecrow = mac && memcmp(mac, MAC_SCARECROW_A1S, 6) == 0 && m.sender == NODE_SCARECROW_A1S;
  ShowFrame f{m.command, static_cast<uint8_t>(fromScarecrow ? NODE_SCARECROW_A1S : 0), m.session_id, m.payload_a};
  portENTER_CRITICAL_ISR(&gShowRxMux);
  size_t next = (gShowRxHead + 1) % SHOW_RX_SLOTS;
  if (next == gShowRxTail) {
    gShowRxDropped = gShowRxDropped + 1;
  } else {
    gShowRx[gShowRxHead] = f;
    gShowRxHead = next;
  }
  portEXIT_CRITICAL_ISR(&gShowRxMux);
}

static void drainShowRx(uint32_t now) {
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
    handleShowFrame(f, now, false);
  }
  static uint32_t reported = 0;
  if (gShowRxDropped != reported) {
    reported = gShowRxDropped;
    logf("[show] WARN rx queue full, %lu frames dropped so far\n", static_cast<unsigned long>(reported));
  }
}

static void initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    logf("[ESP-NOW] init failed (show cues won't arrive; idle loop still runs)\n");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  logf("[ESP-NOW] ready ch=%d\n", ESPNOW_CHANNEL);
}

// Quick (fast) movement = SHOW_ACTIVE run, with a hard backstop.
static bool gQuick = false;
static uint32_t gQuickSinceMs = 0;

static void maintainShow(uint32_t now) {
  logShowOut(gShow.tick(now));
  showfollow::Phase p = gShow.phase();
  if (p != gLastPhase) {
    logf("[show] phase %s -> %s\n", phaseName(gLastPhase), phaseName(p));
    gLastPhase = p;
    gPhaseSinceMs = now;
  }
  bool wantQuick = p == showfollow::Phase::PLAYING;
  if (wantQuick && gQuick && since(now, gQuickSinceMs) >= static_cast<int32_t>(SHOW_QUICK_BACKSTOP_MS)) {
    gShow.abort();
    wantQuick = false;
    logf("[show] WARN quick held %lu s -> backstop, back to ambient\n",
         static_cast<unsigned long>(SHOW_QUICK_BACKSTOP_MS / 1000));
  }
  if (wantQuick != gQuick) {
    gQuick = wantQuick;
    gQuickSinceMs = now;
    logf("[move] -> %s\n", gQuick ? "QUICK (show)" : "AMBIENT");
  }
}

// ===================== Player (JZ-MOS) + call timeline =====================
static bool gPlayerOn = false;
static uint32_t gPlayerOnAtMs = 0;
static uint32_t gPlayerOffUntilMs = 0;  // keep off until this time (clean restart)
static uint8_t gNextCall = 0;
static uint32_t gLastCallAtMs = 0;
static bool gMuted = false;             // bench MUTE
static uint32_t gLoops = 0, gRecycles = 0, gCalls = 0, gStaleCalls = 0;

static void playerPower(bool on, uint32_t now, const char *why) {
  if (on == gPlayerOn) return;
  digitalWrite(PIN_MP3, on ? HIGH : LOW);
  gPlayerOn = on;
  if (on) {
    gPlayerOnAtMs = now;
    gNextCall = 0;
  } else {
    gPlayerOffUntilMs = now + PLAYER_OFF_GAP_MS;
  }
  logf("[mp3] power %s (%s)\n", on ? "ON" : "OFF", why);
}

// Track position in ms (negative while the player is still booting).
static int32_t trackPos(uint32_t now) { return since(now, gPlayerOnAtMs) - static_cast<int32_t>(gBootDelayMs); }

static void cawGesture(uint32_t now, uint16_t durMs) {
  uint32_t hold = constrain(static_cast<uint32_t>(durMs), 300u, 1500u);
  int headSwing = randInt(25, 45) * (random(2) ? 1 : -1);
  int headTarget = static_cast<int>(gHead.angle) + headSwing;
  if (headTarget > gHead.maxA || headTarget < gHead.minA) headTarget = static_cast<int>(gHead.angle) - headSwing;
  axisMove(gHead, headTarget, 220, hold, now, true);
  int tiltSwing = randInt(20, 35) * (gTilt.angle > gTilt.home() ? -1 : 1);
  axisMove(gTilt, static_cast<int>(gTilt.angle) + tiltSwing, 200, hold, now, true);
}

static uint8_t gIdleStep = 0;           // bursts since the last show (drives gap + drowsiness)
static uint8_t gBurstCalls = IDLE_BURST_CALLS_MAX;
static uint32_t gIdleWaitUntilMs = 0;   // idle: no new burst before this
static bool gWasQuick = false;

static uint32_t idleGapMs(uint8_t step) {
  float g = IDLE_GAP_FIRST_MS;
  for (uint8_t i = 0; i < step && g < IDLE_GAP_MAX_MS; ++i) g *= IDLE_GAP_GROWTH;
  g = min(g, static_cast<float>(IDLE_GAP_MAX_MS));
  return static_cast<uint32_t>(g * (random(80, 121) / 100.0f));
}

static void maintainPlayer(uint32_t now) {
  bool narration = gShow.phase() == showfollow::Phase::VISIT;
  bool want = !narration && !gMuted;
  if (gWasQuick && !gQuick) {  // show over: start the idle slow-down from the top
    gIdleStep = 0;
    gIdleWaitUntilMs = now;
  }
  gWasQuick = gQuick;

  if (!want) {
    playerPower(false, now, gMuted ? "muted" : "narration: voices first");
    return;
  }
  if (!gPlayerOn) {
    if (!reached(now, gPlayerOffUntilMs)) return;
    if (gQuick) {
      playerPower(true, now, "play");
    } else if (reached(now, gIdleWaitUntilMs)) {
      gBurstCalls = static_cast<uint8_t>(randInt(IDLE_BURST_CALLS_MIN, IDLE_BURST_CALLS_MAX));
      playerPower(true, now, gBurstCalls == 3 ? "idle burst, 3 calls" : "idle burst, 4 calls");
    }
    return;
  }

  int32_t pos = trackPos(now);
  // End of the loop: power-cycle for a known restart (no drift).
  if (pos >= static_cast<int32_t>(CROW_TRACK_MS)) {
    ++gLoops;
    playerPower(false, now, "end of loop -> restart");
    return;
  }
  // Calls due now -> caw gesture.
  while (gNextCall < CROW_CALL_COUNT && pos >= static_cast<int32_t>(CROW_CALLS[gNextCall].startMs)) {
    const CrowCall &c = CROW_CALLS[gNextCall];
    int32_t late = pos - static_cast<int32_t>(c.startMs);
    if (late <= static_cast<int32_t>(CALL_STALE_MS)) {
      ++gCalls;
      gLastCallAtMs = now;
      logf("[caw] call %u at track %ld ms (late %ld ms)\n", gNextCall, static_cast<long>(pos),
           static_cast<long>(late));
      cawGesture(now, c.durMs);
    } else {
      ++gStaleCalls;
      logf("[caw] WARN call %u skipped, %ld ms late\n", gNextCall, static_cast<long>(late));
    }
    ++gNextCall;
  }
  // Idle: stop after the burst's last call, then wait a little longer each time.
  if (!gQuick) {
    // End after the burst's calls; a call that has already started by then (they overlap in
    // the dense block) is played out too, so a caw is never cut off mid-call.
    uint8_t n = min<uint8_t>(gBurstCalls, CROW_CALL_COUNT);
    uint32_t endMs = 0;
    for (uint8_t i = 0; i < CROW_CALL_COUNT; ++i) {
      if (i >= n && CROW_CALLS[i].startMs > endMs) break;
      endMs = max<uint32_t>(endMs, CROW_CALLS[i].startMs + CROW_CALLS[i].durMs);
    }
    if (pos >= static_cast<int32_t>(endMs + 300)) {
      uint32_t gap = idleGapMs(gIdleStep);
      if (gIdleStep < 255) ++gIdleStep;
      gIdleWaitUntilMs = now + gap;
      playerPower(false, now, "idle burst done");
      logf("[mp3] next idle burst in %lu s (step %u)\n", static_cast<unsigned long>(gap / 1000), gIdleStep);
    }
  }
  // Show: loop only the dense long-call block at the top of the track (CROW_SHOW_BLOCK_MS,
  // from audio/build_track.py; Joe 10-01 "longer crow sounds during active").
  if (gQuick && pos >= static_cast<int32_t>(CROW_SHOW_BLOCK_MS)) {
    ++gRecycles;
    playerPower(false, now, "show: end of the long-call block -> restart");
  }
}

// ===================== Movement =====================
static bool gEventActive = false;  // unused by the glance engine; bench cmds still clear it
static uint32_t gNextEventMs = 0;  // no new glances before this (boot settle + bench pauses; not in the show)
static bool gTiltCockSoon = false;

static int clampAxis(const Axis &a, int d) { return constrain(d, a.minA, a.maxA); }
static int awayFromHome(const Axis &a, int lo, int hi) {  // home +/- lo..hi, a random side
  return clampAxis(a, a.home() + randInt(lo, hi) * (random(2) ? 1 : -1));
}

static void snapTo(Axis &a, int target, uint32_t holdMs, const Mood &m, uint32_t now) {
  axisMove(a, target, snapMs(target - a.angle, m.speed), holdMs, now, true);
}

static void headGlance(const Mood &m, uint32_t now) {
  Axis &a = gHead;
  int total = m.wSmall + m.wBig + m.wDouble + m.wTwitch + m.wRest;
  int r = random(total);
  uint32_t hold = randMs(m.holdMin, m.holdMax);
  bool look = true;
  if ((r -= m.wSmall) < 0) {
    snapTo(a, awayFromHome(a, 10, 35), hold, m, now);
  } else if ((r -= m.wBig) < 0) {  // over the shoulder: the outer part of either side
    int t = random(2) ? randInt(a.minA, a.minA + 20) : randInt(a.maxA - 25, a.maxA);
    snapTo(a, t, hold + randMs(500, 1500), m, now);
  } else if ((r -= m.wDouble) < 0) {  // look, a split-second hold, snap most of the way back
    int t = awayFromHome(a, 30, 60);
    snapTo(a, t, randMs(150, 350), m, now);
    int back = a.home() + static_cast<int>((t - a.home()) * (randInt(20, 40) / 100.0f));
    a.queued = clampAxis(a, back);
    a.qMoveMs = snapMs(a.queued - t, m.speed);
    a.qHoldMs = hold;
  } else if ((r -= m.wTwitch) < 0) {
    snapTo(a, clampAxis(a, static_cast<int>(a.angle) + randInt(3, 8) * (random(2) ? 1 : -1)),
           randMs(300, 1500), m, now);
    look = false;
  } else {  // rest: face forward, still
    snapTo(a, a.home(), randMs(m.restMin, m.restMax), m, now);
    look = false;
  }
  if (look && random(100) < m.tiltCockPct) gTiltCockSoon = true;
}

static void tiltGlance(const Mood &m, uint32_t now) {
  Axis &a = gTilt;
  if (gTiltCockSoon) {  // answer the head's look a beat later
    gTiltCockSoon = false;
    snapTo(a, awayFromHome(a, 15, 40), randMs(m.holdMin, m.holdMax), m, now);
    return;
  }
  int r = random(100);
  if (r < 30) snapTo(a, awayFromHome(a, 15, 40), randMs(m.holdMin, m.holdMax), m, now);  // cock
  else if (r < 60) snapTo(a, a.home(), randMs(m.holdMin, m.holdMax) + 1000, m, now);   // level
  else if (r < 85)
    snapTo(a, clampAxis(a, static_cast<int>(a.angle) + randInt(3, 8) * (random(2) ? 1 : -1)), randMs(400, 1500), m, now);
  else snapTo(a, a.home(), randMs(m.restMin, m.restMax), m, now);  // rest level
}

// Idle gets drowsier with each burst since the show (Joe: "slower to even slower"):
// longer holds and rests, more rests, fewer big looks, softer snaps.
static const Mood &currentMood() {
  if (gQuick) return MOOD_SHOW;
  if (gShow.phase() == showfollow::Phase::VISIT) return MOOD_NARRATION;
  static Mood m;
  float s = min(1.0f, static_cast<float>(gIdleStep) / IDLE_SLEEPY_STEPS);
  m = MOOD_IDLE;
  m.wBig = static_cast<uint8_t>(MOOD_IDLE.wBig * (1.0f - 0.6f * s));
  m.wDouble = static_cast<uint8_t>(MOOD_IDLE.wDouble * (1.0f - 0.6f * s));
  m.wRest = static_cast<uint8_t>(MOOD_IDLE.wRest + 25 * s);
  m.holdMin = static_cast<uint16_t>(MOOD_IDLE.holdMin * (1.0f + 1.5f * s));
  m.holdMax = static_cast<uint16_t>(MOOD_IDLE.holdMax * (1.0f + 1.5f * s));
  m.restMax = static_cast<uint16_t>(min(60000.0f, MOOD_IDLE.restMax * (1.0f + 1.0f * s)));
  m.speed = MOOD_IDLE.speed * (1.0f + 0.5f * s);
  return m;
}

static void maintainMovement(uint32_t now) {
  axisUpdate(gHead, now);
  axisUpdate(gTilt, now);
  if (!gQuick && !reached(now, gNextEventMs)) return;  // boot settle / bench pause
  const Mood &m = currentMood();
  for (Axis *a : {&gHead, &gTilt}) {
    if (!axisIdle(*a, now)) continue;
    if (a->queued >= 0) {  // double-take second half
      int q = a->queued;
      uint32_t mv = a->qMoveMs, hd = a->qHoldMs;
      axisMove(*a, q, mv, hd, now, true);
      continue;
    }
    if (a == &gHead) headGlance(m, now);
    else tiltGlance(m, now);
  }
  // A tilt waiting out a long hold answers the head's look sooner (100-300 ms later).
  if (gTiltCockSoon && !gTilt.moving) {
    uint32_t soon = now + randMs(100, 300);
    if (static_cast<int32_t>(gTilt.holdUntil - soon) > 0) gTilt.holdUntil = soon;
  }
}

// ===================== Eyes =====================
// Joe 2026-10-01: the eyes fade on with movement and off when the crows are still.
// AUTO (boot default) follows the servos; EYES ON / EYES OFF force them for the bench.
enum class EyesMode : uint8_t { AUTO, ON, OFF };
static EyesMode gEyesMode = EyesMode::AUTO;
static ESP32PWM gEyesPwm;
static const uint32_t EYES_PWM_HZ = 1000;
static const uint8_t EYES_PWM_BITS = 10;
static const uint32_t EYES_UP_MS = 200;        // dark -> full when a move starts
static const uint32_t EYES_AFTERGLOW_MS = 400;  // stay full this long after the crows stop
static const uint32_t EYES_DOWN_MS = 1500;     // full -> dark
static float gEyesLevel = 0;                   // 0..1, perceived brightness
static uint32_t gLastMoveMs = 0, gEyesTickMs = 0;

static void maintainEyes(uint32_t now) {
  if (gHead.moving || gTilt.moving) gLastMoveMs = now;
  float target = gEyesMode == EyesMode::ON    ? 1.0f
                 : gEyesMode == EyesMode::OFF ? 0.0f
                 : (since(now, gLastMoveMs) < static_cast<int32_t>(EYES_AFTERGLOW_MS) ? 1.0f : 0.0f);
  float dt = static_cast<float>(constrain(since(now, gEyesTickMs), 0, 200));
  gEyesTickMs = now;
  if (target > gEyesLevel) gEyesLevel = min(target, gEyesLevel + dt / EYES_UP_MS);
  else gEyesLevel = max(target, gEyesLevel - dt / EYES_DOWN_MS);
  float duty = gEyesLevel * gEyesLevel;  // ~gamma 2: the fade looks even to the eye
  gEyesPwm.write(static_cast<uint32_t>(lroundf(duty * ((1u << EYES_PWM_BITS) - 1))));
}

// ===================== Bench =====================
static void printStatus(uint32_t now) {
  const showfollow::Stats &s = gShow.stats();
  logf("[status] idle step=%u nextBurst=%ld s\n", gIdleStep,
       gPlayerOn || gQuick ? 0L : static_cast<long>(max<int32_t>(0, since(gIdleWaitUntilMs, now)) / 1000));
  logf("[status] %s phase=%s move=%s mood=%s mp3=%s pos=%ld ms nextCall=%u/%u delay=%lu ms muted=%d\n",
       FW_VERSION, phaseName(gShow.phase()), gQuick ? "QUICK" : "AMBIENT", currentMood().name,
       gPlayerOn ? "ON" : "OFF", gPlayerOn ? static_cast<long>(trackPos(now)) : 0L, gNextCall, CROW_CALL_COUNT,
       static_cast<unsigned long>(gBootDelayMs), gMuted ? 1 : 0);
  logf("[status] head=GPIO%d %d° tilt=GPIO%d %d° loops=%lu recycles=%lu calls=%lu stale=%lu\n", gHead.pin,
       static_cast<int>(lroundf(gHead.angle)), gTilt.pin, static_cast<int>(lroundf(gTilt.angle)),
       static_cast<unsigned long>(gLoops), static_cast<unsigned long>(gRecycles),
       static_cast<unsigned long>(gCalls), static_cast<unsigned long>(gStaleCalls));
  logf("[status] show plays=%u dup=%u earlyOFF=%u deadline=%u visitTimeout=%u badSender=%u rxDropped=%lu\n",
       s.plays, s.dupIgnored, s.earlyOffIgnored, s.deadlineHits, s.visitTimeouts, s.badSender,
       static_cast<unsigned long>(gShowRxDropped));
}

static void printHelp() {
  logf("[help] SHOW/STATUS | VISIT ACTIVE OFF IDLE STOP FAKE (show cues as the Scarecrow)\n"
       "[help] PLAY (restart the player now) | MUTE | UNMUTE | DELAY <ms> (boot delay, saved)\n"
       "[help] WIGGLE <pin> [n] | SWEEP <pin> [n] [lo] [hi] | SWAP (head<->tilt pins, saved) | HELP\n"
       "[help] EYES AUTO | EYES ON | EYES OFF (LED eyes on GPIO 26) | SERVOS OFF | SERVOS ON | POS <pin> <deg>\n");
}

// Swing or glide one pin with the animation held still.
static void benchServo(int pin, int count, bool sweep, int lo, int hi) {
  Axis *a = pin == gHead.pin ? &gHead : pin == gTilt.pin ? &gTilt : nullptr;
  if (!a) {
    logf("[WIGGLE] GPIO%d is not a crow servo (head=%d tilt=%d)\n", pin, gHead.pin, gTilt.pin);
    return;
  }
  count = constrain(count, 1, 10);
  lo = constrain(lo, 0, 90);
  hi = constrain(hi, 90, 180);
  logf("[WIGGLE] GPIO%d (%s) x%d %s %d-%d\n", pin, a->name, count, sweep ? "sweep" : "wiggle",
       sweep ? lo : 60, sweep ? hi : 120);
  a->moving = false;
  for (int i = 0; i < count; ++i) {
    if (!sweep) {
      a->servo.write(60); delay(700);
      a->servo.write(120); delay(700);
      continue;
    }
    for (int d = 90; d >= lo; --d) { a->servo.write(d); delay(25); }
    logf("[WIGGLE] GPIO%d at min %d°\n", pin, lo);
    delay(500);
    for (int d = lo; d <= hi; ++d) { a->servo.write(d); delay(25); }
    logf("[WIGGLE] GPIO%d at max %d°\n", pin, hi);
    delay(500);
    for (int d = hi; d >= 90; --d) { a->servo.write(d); delay(25); }
  }
  a->servo.write(90);
  a->angle = 90;
  uint32_t now = millis();
  a->holdUntil = now + 3000;
  gEventActive = false;
  gNextEventMs = now + 60000;  // keep still a minute so the next test is easy to see
  logf("[WIGGLE] done, idle moves paused 60 s\n");
}

static void processCommand(char *line, uint32_t now) {
  for (char *p = line; *p; ++p) *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
  char *cmd = line;
  while (*cmd == ' ') ++cmd;
  if (!*cmd) return;
  char *arg = cmd;
  while (*arg && *arg != ' ') ++arg;
  long v[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4 && *arg; ++i) v[i] = strtol(arg, &arg, 10);

  if (!strncmp(cmd, "SHOW", 4) || !strncmp(cmd, "STATUS", 6)) {
    printStatus(now);
  } else if (!strncmp(cmd, "VISIT", 5)) {
    ++gBenchSession;
    handleShowFrame({CMD_SET_THEME, NODE_SCARECROW_A1S, gBenchSession, THEME_CORN}, now, true);
    handleShowFrame({CMD_SHOW_STATE, NODE_SCARECROW_A1S, gBenchSession, SHOW_NARRATOR}, now, true);
  } else if (!strncmp(cmd, "ACTIVE", 6)) {
    handleShowFrame({CMD_START_SCENE, NODE_SCARECROW_A1S, gBenchSession, THEME_CORN}, now, true);
    handleShowFrame({CMD_SHOW_STATE, NODE_SCARECROW_A1S, gBenchSession, SHOW_ACTIVE}, now, true);
  } else if (!strncmp(cmd, "OFF", 3)) {
    handleShowFrame({CMD_SHOW_STATE, NODE_SCARECROW_A1S, gBenchSession, SHOW_OFF}, now, true);
  } else if (!strncmp(cmd, "IDLE", 4)) {
    handleShowFrame({CMD_SHOW_STATE, NODE_SCARECROW_A1S, gBenchSession, SHOW_IDLE}, now, true);
  } else if (!strncmp(cmd, "STOP", 4)) {
    handleShowFrame({CMD_STOP, NODE_SCARECROW_A1S, gBenchSession, 0}, now, true);
  } else if (!strncmp(cmd, "FAKE", 4)) {
    handleShowFrame({CMD_SHOW_STATE, NODE_MAIN_SWITCH, gBenchSession + 1, SHOW_NARRATOR}, now, true);
  } else if (!strncmp(cmd, "PLAY", 4)) {
    gIdleWaitUntilMs = now;  // idle: a burst right away
    playerPower(false, now, "bench PLAY -> restart");
  } else if (!strncmp(cmd, "UNMUTE", 6)) {
    gMuted = false;
    logf("[mp3] unmuted\n");
  } else if (!strncmp(cmd, "MUTE", 4)) {
    gMuted = true;
  } else if (!strncmp(cmd, "DELAY", 5)) {
    if (v[0] >= 0 && v[0] <= 5000 && arg != cmd + 5) {
      gBootDelayMs = static_cast<uint32_t>(v[0]);
      gPrefs.putUInt("delay", gBootDelayMs);
      logf("[mp3] boot delay = %lu ms (saved). PLAY to hear the first call with it.\n",
           static_cast<unsigned long>(gBootDelayMs));
    } else {
      logf("[mp3] DELAY <0-5000 ms>, now %lu\n", static_cast<unsigned long>(gBootDelayMs));
    }
  } else if (!strncmp(cmd, "SWAP", 4)) {
    gSwapped = !gSwapped;
    gPrefs.putBool("swap", gSwapped);
    gHead.servo.detach();
    gTilt.servo.detach();
    attachAxes();
    logf("[PINS] swapped + saved\n");
  } else if (!strncmp(cmd, "WIGGLE", 6)) {
    benchServo(static_cast<int>(v[0]), v[1] > 0 ? static_cast<int>(v[1]) : 2, false, 0, 180);
  } else if (!strncmp(cmd, "SWEEP", 5)) {
    benchServo(static_cast<int>(v[0]), v[1] > 0 ? static_cast<int>(v[1]) : 1, true,
               v[2] > 0 ? static_cast<int>(v[2]) : 30, v[3] > 0 ? static_cast<int>(v[3]) : 150);
  } else if (!strncmp(cmd, "POS", 3)) {  // bench: park one servo at an angle (no range clamp)
    int pin = static_cast<int>(v[0]);
    Axis *ax = pin == gHead.pin ? &gHead : pin == gTilt.pin ? &gTilt : nullptr;
    if (!ax) {
      logf("[POS] POS <pin> <0-180>; head=%d tilt=%d\n", gHead.pin, gTilt.pin);
    } else {
      int deg = constrain(static_cast<int>(v[1]), 0, 180);
      ax->moving = false;
      ax->servo.write(deg);
      ax->angle = deg;
      gEventActive = false;
      gNextEventMs = now + 600000;  // hold still 10 min for mounting
      logf("[POS] GPIO%d (%s) -> %d°, idle moves paused 10 min\n", pin, ax->name, deg);
    }
  } else if (!strncmp(cmd, "SERVOS", 6)) {
    if (strstr(cmd, "OFF")) {  // no signal = servos limp + still; animation writes are ignored while detached
      gHead.servo.detach();
      gTilt.servo.detach();
      logf("[PINS] servos OFF (SERVOS ON to re-attach)\n");
    } else {
      gHead.servo.detach();
      gTilt.servo.detach();
      attachAxes();
      logf("[PINS] servos ON\n");
    }
  } else if (!strncmp(cmd, "EYES", 4)) {
    if (strstr(cmd, "AUTO")) gEyesMode = EyesMode::AUTO;
    else if (strstr(cmd, "OFF")) gEyesMode = EyesMode::OFF;
    else if (strstr(cmd, "ON")) gEyesMode = EyesMode::ON;
    logf("[eyes] GPIO%d %s\n", PIN_EYES,
         gEyesMode == EyesMode::AUTO ? "AUTO (fade with movement)" : gEyesMode == EyesMode::ON ? "ON" : "OFF");
  } else if (!strncmp(cmd, "HELP", 4)) {
    printHelp();
  } else {
    logf("[CMD] unknown '%s'\n", cmd);
    printHelp();
  }
}

static void handleSerial(uint32_t now) {
  static char buf[48];
  static size_t n = 0;
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      buf[n] = '\0';
      processCommand(buf, now);
      n = 0;
    } else if (n + 1 < sizeof(buf)) {
      buf[n++] = c;
    }
  }
}

// ===================== Arduino =====================
void setup() {
  pinMode(PIN_MP3, OUTPUT);  // first thing: player off
  digitalWrite(PIN_MP3, LOW);
  pinMode(PIN_EYES, OUTPUT);  // eyes off until told
  digitalWrite(PIN_EYES, LOW);
  Serial.begin(115200);
  delay(50);
  Serial.println();
  logf("[BOOT] reset_reason=%d\n", static_cast<int>(esp_reset_reason()));
  logf("[BOOT] Shoulder crows %s, track %lu ms, %u calls\n", FW_VERSION,
       static_cast<unsigned long>(CROW_TRACK_MS), CROW_CALL_COUNT);
  randomSeed(esp_random());

  gPrefs.begin("shcrows", false);
  gBootDelayMs = gPrefs.getUInt("delay", DEFAULT_BOOT_DELAY_MS);
  gSwapped = gPrefs.getBool("swap", false);
  logf("[BOOT] boot delay %lu ms, swapped=%d (NVS)\n", static_cast<unsigned long>(gBootDelayMs), gSwapped ? 1 : 0);

  // Brownout guard (bench 2026-09-29: the player + servos on USB power browned the
  // board out every time the player switched on = a reset loop). After a brownout
  // reset the player stays OFF until `UNMUTE`, so a power problem shows up as a
  // WARN, not a loop.
  if (esp_reset_reason() == ESP_RST_BROWNOUT) {
    gMuted = true;
    logf("[mp3] WARN brownout reset -> player kept OFF (check the 5 V supply). UNMUTE to try again.\n");
  }

  initEspNow();
  attachAxes();
  gEyesPwm.attachPin(PIN_EYES, EYES_PWM_HZ, EYES_PWM_BITS);  // after the servos: its own PWM timer
  gEyesPwm.write(0);

  uint32_t now = millis();
  gNextEventMs = now + randMs(5000, 15000);
  gPlayerOffUntilMs = now + 500;  // player on half a second after boot
  printHelp();
}

void loop() {
  uint32_t now = millis();
  handleSerial(now);
  drainShowRx(now);
  maintainShow(now);
  maintainPlayer(now);
  maintainMovement(now);
  maintainEyes(now);
  static uint32_t lastAlive = 0;
  if (since(now, lastAlive) >= 60000) {
    lastAlive = now;
    printStatus(now);
  }
  delay(CONTROL_TICK_MS);
}
