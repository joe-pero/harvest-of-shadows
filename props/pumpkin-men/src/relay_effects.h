#pragma once

#include <Arduino.h>

// ── Hardware pins ─────────────────────────────────────────────────────────────
// Current build of the box (Joe, 2026-09-26 — the 2025 Master Brain map in
// Halloween-Dev/.../Projects/Master/src/config.h is OUT OF DATE for this box).
// MOSFET channels, HIGH = ON. Mapped at the bench by Joe, 2026-09-26:
//   skull jaws:  left = 32   middle = 13   right = 25
//   witch jaws:  middle = 18   right = 14      (left witch head has NO jaw)
//   witch eyes = 4: all witch-head eye lights, candle-flicker PWM, ON for
//                the whole theme track only (off when idle / action clips)
//   fog  = 27   (fog machines 1 AND 2 both on this one channel)
//   reaching men = 26: HIGH = running, LOW = off (bench-verified). ON only during a theme.
//   spare = 33 (held LOW)      skeleton: REMOVED from this build
//   MP3 RX = 16, TX = 17 (mp3_player.h)      PIR = 36 only (pir_sensor.h)
//   GPIO 19 and 23 are DAMAGED on this board — never use them.
#define JAW_COUNT        5
#define PIN_JAW_SKULL_L  32
#define PIN_JAW_SKULL_M  13
#define PIN_JAW_SKULL_R  25
#define PIN_JAW_WITCH_M  18
#define PIN_JAW_WITCH_R  14
#define PIN_WITCH_EYES    4
#define PIN_FOG         27
#define PIN_REACHING_MEN 26
// Reaching men run for the WHOLE theme track only, off otherwise (Joe 2026-09-26).
// Bench-verified 2026-09-26: pin HIGH = reaching men RUN, LOW = off (the first
// guess from "normally closed" was backwards — HIGH at idle left them stuck on).
#define REACH_ON_LEVEL   HIGH
#define REACH_OFF_LEVEL  LOW
#define PIN_SPARE       33

// ── Effect timing ─────────────────────────────────────────────────────────────
// Ported from the 2025 JAW_ENABLE_MID / PUMPKIN_RELAYS / FOG / SKELETON
// constants, decoupled from the old MID-zone/PIR framing (no zones anymore —
// this now just runs for the duration of whichever theme track is playing).
// These are first-pass defaults, not tuned against Joe's actual 3 tracks yet —
// re-check timing once real audio is on the SD card and the show has been
// heard end to end.

// Fog (Joe, 2026-09-26): one 36 s burst per minute of the track, starting at a
// random point in that minute (re-rolled every track start) so the whole burst
// fits inside its minute: 3 bursts in a 3:00 theme. The relay holds the fog
// machines' button; the heater needs a few seconds to register, hence long
// holds. A burst past the track's end is cut by the length stop (2:09 Mutant).
#define FOG_BURST_COUNT     3
#define FOG_SLOT_MS     60000     // one burst per slot (minute)
#define FOG_BURST_ON_MS 36000
#define FOG_BENCH_MS      3000    // bench `fog` pulse (wiring check only)
// Fog cooldown (Joe, 2026-10-27): no burst within 60 s of the last theme's end.
// A burst that comes due sooner waits for the cooldown, then runs.
#define FOG_COOLDOWN_MS  60000

// Jaw scheduler (Joe, 2026-09-26: "random and lots of movement", then "more
// chaos and faster"): ROLLING, not group bursts. Every JAW_STEP_MIN..MAX ms the
// next jaw is dealt from a Fisher-Yates deck of all 5 (skulls + witches: every
// jaw gets a turn before any repeats, fresh shuffle each track) and runs for its
// own random JAW_MIN..MAX_ON_MS, so jaws overlap and stagger. At most
// JAW_MAX_ACTIVE move at once (a dealt jaw that's already moving is skipped).
#define JAW_START_DELAY_MS   1000  // let the track's audio actually start first
#define JAW_CLIP_START_DELAY_MS 300 // short action clips (4–11 s): start almost at once
#define JAW_STEP_MIN_MS       150  // gap between one jaw starting and the next
#define JAW_STEP_MAX_MS       500
#define JAW_MIN_ON_MS         600  // how long one jaw moves
#define JAW_MAX_ON_MS        2000
#define JAW_MAX_ACTIVE          4  // of 5

// Witch eyes candle flicker: a new random brightness every 40–130 ms, eased
// toward, with an occasional deeper dip (a gust on the flame).
#define EYES_LEDC_CH           4
#define EYES_MIN_LEVEL        90   // 0..255
#define EYES_MAX_LEVEL       255

enum class Fx : uint8_t { THEME, CLIP };
// THEME = one of the 3 theme tracks: reaching men + witch eyes flicker + fog + jaws
// CLIP  = a short action clip: jaws only
uint32_t relayLastSwitchMs();     // millis() of the last output change (PIR blanking)
void relayEffectsInit();          // pinMode + all-off; call once from setup()
// Arm the timers; call when a track starts. lengthMs = the track's known audio
// length: every effect stops right there, even if the MP3 module's end-of-track
// message is late or missing (the driver's own STOP waits length + 2 s grace).
void relayEffectsStart(Fx mode, uint32_t lengthMs);
void relayEffectsTick();          // drive the timers; call every loop while a track may be playing
void relayEffectsStop();          // force every relay off now; call on track end / CMD_STOP

// Bench test — fire one relay in isolation for wiring verification, ignoring
// the scheduler. Used by main.cpp's serial bench-test commands.
void relayEffectsBenchJaw(uint8_t which);  // which = 1..JAW_COUNT (order in jawName())
void relayEffectsJawLog(bool on);         // print every jaw start (noisy)
void relayEffectsBenchEyes();             // witch eyes flicker 5 s
const char* jawName(uint8_t idx);         // 0-based
void relayEffectsBenchFog();
void relayEffectsBenchReach();            // reaching men ON 5 s (bench)
