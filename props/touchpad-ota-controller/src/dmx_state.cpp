#include "dmx_state.h"
#include "dmx_timeline.h"
#include "pins.h"
#include <math.h>

// ── LEDC (arduino-esp32 2.0.x) — common-anode LED: ON = LOW = inverted duty ──
static const uint32_t LED_FREQ = 20000;
static const uint8_t  LED_RES  = 8;
static const uint32_t LED_MAX  = 255;
static const uint8_t  CH_R = 0, CH_G = 1, CH_B = 2;

static uint8_t chanFor(uint8_t pin) {
    if (pin == LED_R_PIN) return CH_R;
    if (pin == LED_G_PIN) return CH_G;
    return CH_B;
}
static void ledSet(uint8_t pin, uint8_t bright) { ledcWrite(chanFor(pin), LED_MAX - bright); }
static void rgb(uint8_t r, uint8_t g, uint8_t b) {
    ledSet(LED_R_PIN, r); ledSet(LED_G_PIN, g); ledSet(LED_B_PIN, b);
}

// ── state ───────────────────────────────────────────────────────────────────
static DmxTimingMode s_timingMode  = DMX_TIMING_EXTERNAL;
static bool          s_running     = false;
static uint32_t      s_startMs     = 0;

void dmx_setup() {
    ledcSetup(CH_R, LED_FREQ, LED_RES);  ledcAttachPin(LED_R_PIN, CH_R);
    ledcSetup(CH_G, LED_FREQ, LED_RES);  ledcAttachPin(LED_G_PIN, CH_G);
    ledcSetup(CH_B, LED_FREQ, LED_RES);  ledcAttachPin(LED_B_PIN, CH_B);
    rgb(0, 0, 0);
    s_running = false;
}

void dmx_set_timing_mode(DmxTimingMode m) { s_timingMode = m; }
DmxTimingMode dmx_timing_mode()           { return s_timingMode; }

void dmx_segment_start() {
    if (s_running) return;                 // idempotent vs the 3x CMD_SEGMENT_START
    s_running = true;
    s_startMs = millis();
    Serial.printf("[dmx] segment START (timing=%s, ~%lu s show)\n",
                  s_timingMode == DMX_TIMING_LOCAL ? "LOCAL" : "EXTERNAL",
                  (unsigned long)(DMX_T_END_MS / 1000));
}

void dmx_segment_start_at(uint32_t offset_ms) {
    if (s_running) return;
    s_running = true;
    s_startMs = millis() - offset_ms;      // pretend offset_ms has already elapsed
    Serial.printf("[dmx] segment START @ +%lu s (bench)\n", (unsigned long)(offset_ms / 1000));
}

void dmx_segment_end() {
    if (!s_running) return;
    s_running = false;
    Serial.println(F("[dmx] segment END -> idle green"));
}

bool     dmx_segment_running() { return s_running; }
uint32_t dmx_elapsed_ms()      { return s_running ? (millis() - s_startMs) : 0; }

DmxPhase dmx_phase() {
    if (!s_running) return DMX_IDLE_GREEN;
    uint32_t t = millis() - s_startMs;
    if (t >= DMX_T_END_MS)         return DMX_IDLE_GREEN;
    if (t >= DMX_T_GREEN_CLOSE_MS) return DMX_GREEN_CLOSE;
    if (t >= DMX_T_RED_END_MS)     return DMX_DARK_GAP;
    if (t >= DMX_T_RED_START_MS)   return DMX_RED;
    if (t >= DMX_T_GREEN_OPEN_MS)  return DMX_RED;     // 1:13.10-1:13.40 flutter -> treat as RED entering
    if (t >= DMX_T_INTRO_DARK_MS)  return DMX_GREEN_OPEN;
    return DMX_INTRO_DARK;
}

bool dmx_strobe_window() {
    if (!s_running) return false;
    uint32_t t = millis() - s_startMs;
    return t >= DMX_T_STROBE_START_MS && t < DMX_T_STROBE_END_MS;
}

// ── LED rendering ───────────────────────────────────────────────────────────
// GREEN-DOMINANT "water" wash with only a HINT of blue (Joe, 2026-09-07 — "more
// green, only slight blue"). Offset dual-sine so green and blue drift out of
// phase (a wash, not a solid). Reference: Eave's green + sky-blue water effect,
// but this LED reads greener. Approximate match, not a live sync.
// BLUE_PEAK is the max blue value; keep it well under the green swing.
static const uint8_t GREEN_WATER_BLUE_PEAK = 22;
static void renderGreenWater(uint32_t nowMs, uint8_t gBase, uint8_t bBase) {
    float s = nowMs / 1000.0f;
    float wg = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * 0.28f * s);
    float wb = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * 0.22f * s + 1.1f);
    rgb(0, (uint8_t)(gBase + (255 - gBase) * 0.65f * wg),
           (uint8_t)(bBase + GREEN_WATER_BLUE_PEAK * wb));
}

// White electrical-fire strobe (Joe, 2026-09-07). Narrative: after the zombie
// breaks the window and runs around (~1:20, air cannon + HV flash), the white
// kicks in — "random broken electrical fire, never on long, more flashing,
// more randomness, breaks in the white."
//
// Model = a broken arcing wire: rapid short white flashes (8-46 ms — NEVER solid)
// with mostly-tiny stutter gaps between them (a near-continuous nervous flicker),
// punctuated by medium gaps, real breaks (fractions of a second to ~2.5 s), and
// the odd long dead break (3-6.5 s). Every off-interval is drawn from one of
// four regimes at random, so it never settles into a rhythm.
// The RED warning light continues underneath (redBed) — never a red+white blend.
static void strobeOverlay(uint32_t nowMs, uint8_t redBed) {
    static uint32_t until = 0;
    static bool     on    = false;

    if (nowMs >= until) {
        uint32_t r = esp_random();
        if (on) {
            on = false;
            uint32_t k = r % 1000;
            if      (k < 550) until = nowMs + 10 + (r % 46);      // 55%: 10-55 ms  stutter
            else if (k < 850) until = nowMs + 60 + (r % 220);     // 30%: 60-280 ms
            else if (k < 970) until = nowMs + 350 + (r % 2200);   // 12%: 0.35-2.55 s break
            else              until = nowMs + 3000 + (r % 3500);  //  3%: 3-6.5 s dead
        } else {
            on = true;
            until = nowMs + 8 + (r % 39);                         // 8-46 ms — never on long
        }
    }
    if (on) rgb(255, 255, 255);
    else    rgb(redBed, 0, 0);
}

static uint8_t redHeartbeat(uint32_t nowMs) {
    // double-thump "lub-dub" (~70 bpm): pulse at 0, second at ~0.30 s, low bed between
    const float P = 0.857f;
    float t = fmodf(nowMs / 1000.0f, P);
    auto thump = [](float x) -> float {
        const float W = 0.13f;
        return (x < 0 || x > W) ? 0.0f : 0.5f - 0.5f * cosf(2.0f * (float)M_PI * x / W);
    };
    float e = thump(t) + thump(t - 0.30f);
    if (e > 1.0f) e = 1.0f;
    return (uint8_t)(25 + 230 * e);
}

// GLASS BREAK — solid fast regular white strobe (~12 Hz), full brightness.
static void renderSolidStrobe(uint32_t nowMs) {
    rgb((nowMs / 42) & 1 ? 255 : 0, (nowMs / 42) & 1 ? 255 : 0, (nowMs / 42) & 1 ? 255 : 0);
}

// DOUSE (2026-09-21, Joe) — incineration fluid being dumped on Specimen 04.
// A heavier, more urgent amber "flooding" pulse — same sine-wave shape as the
// green water wash, but amber/orange-toned and pulsing faster to read as
// liquid actively pouring rather than a calm ambient wash.
static void renderDouseWash(uint32_t nowMs) {
    float s = nowMs / 1000.0f;
    float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * 0.9f * s);   // ~1.1 s cycle
    rgb((uint8_t)(120 + 135 * w), (uint8_t)(60 + 70 * w), 0);
}

// INCINERATE (2026-09-21, Joe) — fire actively purging the specimen. Unlike
// the BROKEN electrical strobe (real dark gaps, white-on-red), real fire never
// goes fully dark — it's a continuous flicker between deep ember and bright
// flame. Random-walk brightness/color mix, always at least a dim ember glow.
static void renderFireFlicker(uint32_t nowMs) {
    static uint32_t nextMs = 0;
    static uint8_t  level  = 140;   // 0=dim ember .. 255=bright flame
    if (nowMs >= nextMs) {
        int16_t delta = (int16_t)(esp_random() % 121) - 50;   // -50..+70, biased up (lively)
        int16_t v = (int16_t)level + delta;
        level = (uint8_t)(v < 60 ? 60 : (v > 255 ? 255 : v));   // never fully dark
        nextMs = nowMs + 30 + (esp_random() % 55);              // 30-85 ms per flicker step
    }
    // Flame color: red always near-full, green rides with brightness (orange<->yellow-white),
    // a little blue only at the very brightest flickers (hot flame core).
    uint8_t g = (uint8_t)((uint16_t)level * 130 / 255);
    uint8_t b = level > 210 ? (uint8_t)((level - 210) * 3) : 0;
    rgb(level, g, b);
}

// ── bench: force one of the three states for tuning ─────────────────────────
static int s_forced = -1;   // -1 normal, 0 green, 1 red, 2 white
void dmx_force_state(int s) { s_forced = s; }
int  dmx_forced_state()     { return s_forced; }

int dmx_show_state() {
    if (s_forced >= 0) return s_forced;                  // 0..6
    if (!s_running)    return HS_CONTAINED;
    uint32_t t = millis() - s_startMs;
    if (t >= DMX_T_END_MS)             return HS_CONTAINED;
    // 2026-09-21 (Joe): the tail (2:49.86-3:03.92) is now just the plain
    // CONTAINED/safe screen instead of a distinct RESET look — no separate
    // check needed here since HS_CONTAINED is exactly what falls out below
    // once DMX_T_INCINERATE_START's window ends at DMX_T_GREEN_CLOSE_MS.
    if (t >= DMX_T_GREEN_CLOSE_MS)     return HS_CONTAINED;
    if (t >= DMX_T_INCINERATE_START_MS) return HS_INCINERATE;
    if (t >= DMX_T_DOUSE_START_MS)     return HS_DOUSE;
    if (t >= DMX_T_GLASSBREAK_END_MS)  return HS_BROKEN;
    if (t >= DMX_T_STROBE_START_MS)    return HS_GLASSBREAK;
    if (t >= DMX_T_WARNING_START_MS)   return HS_WARNING;
    return HS_CONTAINED;
}

float dmx_escalation() {
    switch (dmx_show_state()) {
        case HS_CONTAINED: return 0.0f;
        case HS_RESET: {
            if (!s_running) return 0.0f;
            float e = (float)((millis() - s_startMs) - DMX_T_GREEN_CLOSE_MS)
                    / (float)(DMX_T_END_MS - DMX_T_GREEN_CLOSE_MS);
            return 1.0f - (e < 0 ? 0 : (e > 1 ? 1 : e));   // 1 -> 0 over the reset
        }
        case HS_WARNING: {
            if (!s_running) return 0.4f;
            float e = (float)((millis() - s_startMs) - DMX_T_WARNING_START_MS)
                    / (float)(DMX_T_STROBE_START_MS - DMX_T_WARNING_START_MS);
            return 0.35f + 0.5f * (e < 0 ? 0 : (e > 1 ? 1 : e));
        }
        default: return 1.0f;                 // glass break / broken / douse / incinerate
    }
}

void dmx_tick(uint32_t nowMs) {
    switch (dmx_show_state()) {
        case HS_CONTAINED:
        case HS_RESET:       renderGreenWater(nowMs, s_running || s_forced >= 0 ? 120 : 60, 24); break;
        case HS_WARNING:     rgb(redHeartbeat(nowMs), 0, 0);          break;
        case HS_GLASSBREAK:  renderSolidStrobe(nowMs);                break;
        case HS_BROKEN:      strobeOverlay(nowMs, redHeartbeat(nowMs) / 3); break; // broken white + red behind
        case HS_DOUSE:       renderDouseWash(nowMs);                 break;
        case HS_INCINERATE:  renderFireFlicker(nowMs);                break;
    }
    if (s_running && (millis() - s_startMs) >= DMX_T_END_MS) dmx_segment_end();
}
