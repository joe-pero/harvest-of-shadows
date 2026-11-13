#pragma once
// =============================================================================
// Zombie Container HUD — "DARROW CHEMICAL CO. // 2-4-5 TRIOXIN" containment
// monitor on the Hosyond 4" ST7796S. Rendered on the ESP32 (no video / no SD).
// Phase-reactive: GREEN (contained) / RED (alert) / WHITE (specimen loose),
// driven by dmx_show_state() + dmx_escalation().
// =============================================================================
#include <Arduino.h>

void hud_setup();          // init the panel + backlight; call once
void hud_tick();           // call every loop — renders a frame (~15-20 fps)
void hud_set_enabled(bool on);
void hud_force_idle(bool on);   // bench: force the dim wandering STANDBY screensaver

// Burn-in mitigation (always on): a slow +/-6 px "orbit" shifts the whole layout
// every 90 s; CONTAINED/RESET dim the backlight; after 3 min continuously
// CONTAINED the panel drops to a dim STANDBY card that wanders the full screen.

// =============================================================================
// Touch override (2026-09-21 redesign) — single full-screen manual FIRE button.
// =============================================================================
// Normal operation needs zero touch: the auto-fire path (ESP-NOW SET_THEME =
// THEME_ROTLD, armed by default) is the whole show trigger. This override
// exists only as a manual fallback — start the show yourself if the ESP-NOW
// cue doesn't land, or just want to trigger it directly.
//
// Flow, two full-screen taps, no split zones anywhere:
//   1. Tap ANYWHERE on the containment monitor (idle HUD) -> opens the
//      override screen (a single "TAP TO FIRE" prompt covering the whole
//      480x320 panel).
//   2. Tap ANYWHERE on that screen -> fires (relay pulse + DMX segment +
//      CMD_SEGMENT_START broadcast), jumps back to the monitor to watch it run.
//   No tap within MENU_TIMEOUT_MS -> silently returns to the monitor, no fire.
// No ARM/DISARM toggle and no Singing Pumpkins jukebox on this panel anymore
// (2026-09-21, Joe — dropped after the touch-calibration session; arm/disarm
// remain serial-only commands in main.cpp, same as estop already was).
//
// LOCKOUT: while dmx_segment_running() is true, the override screen cannot be
// opened at all — hud_tick() ignores touch entirely and only ever shows the
// live phase-reactive HUD (CONTAINED/WARNING/GLASS BREAK/BROKEN/RESET), in
// sync with the running show. No on-screen e-stop (2026-09-19, Joe): once
// triggered, the PicoDMX runs its own ~184 s show with no wire back to this
// panel — nothing here can actually interrupt it. `estop` stays a
// serial-only bench/ops command (main.cpp) that resets the mesh's segment
// state.
//
// Register the callback once in setup(), before the first hud_tick():
typedef void (*ZombieFireFn)();                        // manual fire, confirmed on-screen

void hud_set_zombie_callbacks(ZombieFireFn fire);

// BENCH (2026-09-19): live-adjust the touch driver's offset_rotation (0-7) without
// reflashing, to find the value that matches this panel's physical touch-overlay
// orientation vs. the LCD's setRotation(3). Takes effect on the very next tap.
void hud_set_touch_rotation(uint8_t r);

// BENCH (2026-09-21): freezes the real override screen — taps never fire or
// navigate, just get logged (position only; the whole screen is one region,
// there's nothing left to classify). Lets you hammer the screen and watch
// [freeze] lines without risking a real relay pulse.
void hud_start_freezetest();
