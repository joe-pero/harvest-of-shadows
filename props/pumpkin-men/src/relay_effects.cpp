#include "relay_effects.h"
#include "BlackBox.h"

static void printTs() {
    uint32_t ms = millis();
    Log.printf("[%02u:%02u.%03u] ", ms / 60000, (ms % 60000) / 1000, ms % 1000);
}

// Every output change is time-stamped: the PIR ignores detections that start
// right after one (switching noise reached GPIO36, bench 2026-09-26).
static uint32_t s_lastSwitchMs = 0;
uint32_t relayLastSwitchMs() { return s_lastSwitchMs; }
static inline void outWrite(uint8_t pin, uint8_t v) { digitalWrite(pin, v); s_lastSwitchMs = millis(); }

static const uint8_t s_jawPins[JAW_COUNT] = { PIN_JAW_SKULL_L, PIN_JAW_SKULL_M, PIN_JAW_SKULL_R,
                                              PIN_JAW_WITCH_M, PIN_JAW_WITCH_R };
static const char* const s_jawNames[JAW_COUNT] = { "skullL", "skullM", "skullR", "witchM", "witchR" };
const char* jawName(uint8_t idx) { return idx < JAW_COUNT ? s_jawNames[idx] : "?"; }

// ── Witch eyes: candle flicker on LEDC PWM ───────────────────────────────────
static bool     s_jawLog     = false;            // bench `jawlog`: print every jaw start
void relayEffectsJawLog(bool on) { s_jawLog = on; }
static bool     s_eyesOn     = false;
static uint32_t s_eyesNextMs = 0;
static int16_t  s_eyesLevel  = 0, s_eyesTarget = 0;

static void eyesSet(bool on) {
    s_eyesOn = on; s_lastSwitchMs = millis();
    s_eyesLevel = on ? EYES_MAX_LEVEL : 0; s_eyesTarget = s_eyesLevel; s_eyesNextMs = 0;
    ledcWrite(EYES_LEDC_CH, s_eyesLevel);
}

static void eyesTick(uint32_t now) {
    if (!s_eyesOn) return;
    if ((int32_t)(now - s_eyesNextMs) < 0) return;
    s_eyesNextMs = now + 15;                                   // ease step ~66 Hz
    static uint32_t s_newTargetMs = 0;
    if ((int32_t)(now - s_newTargetMs) >= 0) {
        s_eyesTarget  = (random(12) == 0) ? random(EYES_MIN_LEVEL / 2, EYES_MIN_LEVEL + 20)   // gust dip
                                          : random(EYES_MIN_LEVEL + 40, EYES_MAX_LEVEL + 1);
        s_newTargetMs = now + 40 + random(91);
    }
    s_eyesLevel += (s_eyesTarget - s_eyesLevel) / 3;
    ledcWrite(EYES_LEDC_CH, constrain(s_eyesLevel, 0, 255));
}

static bool     s_active            = false;
static Fx       s_mode              = Fx::THEME;
static uint32_t s_startMs           = 0;
static uint32_t s_lengthMs          = 0;      // effects end here (0 = no limit)

static uint8_t  s_fogNext           = 0;      // next burst
static uint32_t s_fogAt[FOG_BURST_COUNT];
static uint8_t  s_fogCount = FOG_BURST_COUNT;   // bursts that fit this track     // burst starts after track start (random per track)
static uint32_t s_fogOffAtMs        = 0;
static uint32_t s_lastThemeEndMs    = 0;      // 0 = no theme since boot (fog cooldown)
static bool     s_fogWaitLogged     = false;

static void noteThemeEnd() {
    if (s_active && s_mode == Fx::THEME) { s_lastThemeEndMs = millis(); if (!s_lastThemeEndMs) s_lastThemeEndMs = 1; }
}

// Jaw scheduler state (rolling)
static uint32_t s_jawNextEventMs    = 0;          // next jaw start
static bool     s_jawOn[JAW_COUNT]  = {};
static uint32_t s_jawOffAt[JAW_COUNT] = {};

// ── Jaw shuffle deck (Fisher-Yates, same idea as the Tree's action deck) ─────
static uint8_t  s_jawDeck[JAW_COUNT] = { 0, 1, 2, 3, 4 };
static uint8_t  s_jawDeckPos        = JAW_COUNT; // exhausted -> shuffle on next deal
static int8_t   s_lastJawDealt      = -1;

static void shuffleJawDeck() {
    for (uint8_t i = JAW_COUNT - 1; i > 0; i--) {
        uint8_t j = (uint8_t)random(i + 1);
        uint8_t t = s_jawDeck[i]; s_jawDeck[i] = s_jawDeck[j]; s_jawDeck[j] = t;
    }
    s_jawDeckPos = 0;
    // No repeat across the reshuffle boundary.
    if (s_jawDeck[0] == s_lastJawDealt) {
        uint8_t k = 1 + (uint8_t)random(JAW_COUNT - 1);
        uint8_t t = s_jawDeck[0]; s_jawDeck[0] = s_jawDeck[k]; s_jawDeck[k] = t;
    }
}

// Next jaw from the deck that isn't already on in this burst.
static uint8_t dealJaw() {
    if (s_jawDeckPos >= JAW_COUNT) shuffleJawDeck();
    for (uint8_t k = s_jawDeckPos + 1; s_jawOn[s_jawDeck[s_jawDeckPos]] && k < JAW_COUNT; k++) {
        uint8_t t = s_jawDeck[s_jawDeckPos]; s_jawDeck[s_jawDeckPos] = s_jawDeck[k]; s_jawDeck[k] = t;
    }
    uint8_t idx   = s_jawDeck[s_jawDeckPos++];
    s_lastJawDealt = (int8_t)idx;
    return idx;
}

static void allJawsOff() {
    for (uint8_t i = 0; i < JAW_COUNT; i++) {
        outWrite(s_jawPins[i], LOW);
        s_jawOn[i] = false;
    }
}

void relayEffectsInit() {
    for (uint8_t i = 0; i < JAW_COUNT; i++) pinMode(s_jawPins[i], OUTPUT);
    pinMode(PIN_FOG, OUTPUT);
    ledcSetup(EYES_LEDC_CH, 5000, 8);
    ledcAttachPin(PIN_WITCH_EYES, EYES_LEDC_CH);
    pinMode(PIN_SPARE, OUTPUT);        outWrite(PIN_SPARE, LOW);
    pinMode(PIN_REACHING_MEN, OUTPUT); outWrite(PIN_REACHING_MEN, REACH_OFF_LEVEL);   // on only during a theme
    relayEffectsStop();
    printTs(); Log.println("[relay] Init — 5 jaws + witch eyes + fog + reaching men off");
}

void relayEffectsStart(Fx mode, uint32_t lengthMs) {
    noteThemeEnd();                           // a theme replaced mid-run counts as ended
    const bool full   = mode != Fx::CLIP;
    s_active          = true;
    s_lengthMs        = lengthMs;
    s_mode            = mode;
    s_startMs         = millis();
    s_fogNext         = 0;
    s_fogOffAtMs      = 0;
    s_fogWaitLogged   = false;
    for (uint8_t i = 0; i < FOG_BURST_COUNT; i++)
        s_fogAt[i] = i * FOG_SLOT_MS + (uint32_t)random(FOG_SLOT_MS - FOG_BURST_ON_MS + 1);
    // A burst that can't run its full 36 s before the track ends is skipped
    // (Joe: no 3rd burst on the 2:09 Mutant theme).
    s_fogCount = FOG_BURST_COUNT;
    if (lengthMs)
        while (s_fogCount && (s_fogCount - 1) * FOG_SLOT_MS + FOG_BURST_ON_MS > lengthMs) s_fogCount--;
    s_jawNextEventMs  = s_startMs + (full ? JAW_START_DELAY_MS : JAW_CLIP_START_DELAY_MS);
    s_jawDeckPos      = JAW_COUNT;            // fresh shuffle every track start
    allJawsOff();
    outWrite(PIN_FOG, LOW);
    eyesSet(mode == Fx::THEME);
    outWrite(PIN_REACHING_MEN, mode == Fx::THEME ? REACH_ON_LEVEL : REACH_OFF_LEVEL);
    printTs(); Log.printf("[relay] Effects armed for this track (%s)\n",
                             mode == Fx::THEME ? "reaching men + witch eyes + fog + jaws" : "jaws only");
}

void relayEffectsStop() {
    noteThemeEnd();
    s_active = false;
    allJawsOff();
    outWrite(PIN_FOG, LOW);
    eyesSet(false);
    outWrite(PIN_REACHING_MEN, REACH_OFF_LEVEL);
}

void relayEffectsTick() {
    if (!s_active) return;
    const uint32_t now     = millis();
    const uint32_t elapsed = now - s_startMs;
    const bool     s_full  = s_mode != Fx::CLIP;
    if (s_lengthMs && elapsed >= s_lengthMs) {           // audio is over: everything off now
        relayEffectsStop();
        printTs(); Log.printf("[relay] Effects off at track length (%lu.%lus)\n",
                                 (unsigned long)(s_lengthMs / 1000), (unsigned long)((s_lengthMs % 1000) / 100));
        return;
    }
    eyesTick(now);

    // ── Fog: one 36 s burst at a random point in each minute ───────────────
    const int32_t fogWait = s_lastThemeEndMs ? (int32_t)(s_lastThemeEndMs + FOG_COOLDOWN_MS - now) : 0;
    if (s_full && s_fogOffAtMs == 0 && s_fogNext < s_fogCount &&
        elapsed >= s_fogAt[s_fogNext] && fogWait > 0) {
        if (!s_fogWaitLogged) {
            printTs(); Log.printf("[relay] Fog burst %u held: cooldown after the last theme, %lds left\n",
                                  (unsigned)(s_fogNext + 1), (long)((fogWait + 999) / 1000));
            s_fogWaitLogged = true;
        }
    } else if (s_full && s_fogOffAtMs == 0 && s_fogNext < s_fogCount &&
        elapsed >= s_fogAt[s_fogNext]) {
        s_fogWaitLogged = false;
        s_fogOffAtMs = now + FOG_BURST_ON_MS;
        outWrite(PIN_FOG, HIGH);
        printTs(); Log.printf("[relay] Fog ON (burst %u/%u at %lu.%lus, %lus)\n", (unsigned)(s_fogNext + 1),
                                 (unsigned)s_fogCount, (unsigned long)(elapsed / 1000),
                                 (unsigned long)((elapsed % 1000) / 100), (unsigned long)(FOG_BURST_ON_MS / 1000));
        s_fogNext++;
    }
    if (s_fogOffAtMs > 0 && (int32_t)(now - s_fogOffAtMs) >= 0) {
        outWrite(PIN_FOG, LOW);
        s_fogOffAtMs = 0;
        printTs(); Log.println("[relay] Fog OFF");
    }

    // ── Jaws: rolling, dealt from the shuffle deck ─────────────────────────
    uint8_t active = 0;
    for (uint8_t i = 0; i < JAW_COUNT; i++) {
        if (s_jawOn[i] && (int32_t)(now - s_jawOffAt[i]) >= 0) {
            outWrite(s_jawPins[i], LOW);
            s_jawOn[i] = false;
        }
        if (s_jawOn[i]) active++;
    }
    if ((int32_t)(now - s_jawNextEventMs) >= 0) {
        s_jawNextEventMs = now + JAW_STEP_MIN_MS + (uint32_t)random(JAW_STEP_MAX_MS - JAW_STEP_MIN_MS + 1);
        if (active < JAW_MAX_ACTIVE) {
            uint8_t idx = dealJaw();                     // skips jaws already moving
            if (!s_jawOn[idx]) {
                uint32_t onMs   = JAW_MIN_ON_MS + (uint32_t)random(JAW_MAX_ON_MS - JAW_MIN_ON_MS + 1);
                s_jawOn[idx]    = true;
                s_jawOffAt[idx] = now + onMs;
                outWrite(s_jawPins[idx], HIGH);
                if (s_jawLog) { printTs(); Log.printf("[relay] jaw %s %lums (%u moving)\n", s_jawNames[idx],
                                                         (unsigned long)onMs, (unsigned)(active + 1)); }
            }
        }
    }
}

// ── Bench test helpers — ignore the scheduler, drive one relay directly ──────

void relayEffectsBenchJaw(uint8_t which) {
    if (which < 1 || which > JAW_COUNT) return;
    uint8_t pin = s_jawPins[which - 1];
    printTs(); Log.printf("[relay] Bench: jaw %u (%s, GPIO%u) pulse\n", which, s_jawNames[which - 1], (unsigned)pin);
    outWrite(pin, HIGH);
    delay(800);
    outWrite(pin, LOW);
}

void relayEffectsBenchFog() {
    printTs(); Log.println("[relay] Bench: fog pulse");
    outWrite(PIN_FOG, HIGH);
    delay(FOG_BENCH_MS);
    outWrite(PIN_FOG, LOW);
}

void relayEffectsBenchReach() {
    printTs(); Log.println("[relay] Bench: reaching men ON 5 s (GPIO26 = REACH_ON_LEVEL)");
    outWrite(PIN_REACHING_MEN, REACH_ON_LEVEL);
    delay(5000);
    outWrite(PIN_REACHING_MEN, REACH_OFF_LEVEL);
    printTs(); Log.println("[relay] Bench: reaching men OFF");
}

void relayEffectsBenchEyes() {
    printTs(); Log.println("[relay] Bench: witch eyes candle flicker 5 s");
    eyesSet(true);
    uint32_t until = millis() + 5000;
    while ((int32_t)(until - millis()) > 0) { eyesTick(millis()); delay(5); }
    eyesSet(false);
}
