#pragma once
// =============================================================================
// Skeleton jaws (Joe 2026-10-02) — two 5 V talking skeletons beside the pumpkins
// =============================================================================
// Power on = jaw opens, power off = jaw drops with a CLUNK. A 2-ch relay module
// (12 V coil board, high-level trigger, ground shared with the ESP32) switches each
// skeleton's 5 V: GPIO25 = left, GPIO26 = right (the 2025 FX relay pins).
//
// A rare extra, not lip sync. The card sends each clip's candidate moments right
// after NOW (`CUES <n> <groups>`, built offline from the audio by sdcard/skel_cues.py):
//   H<B|L|R><start>-<end>   hold: jaw open over a loud peak, clunk at its end
//   K<B|L|R><t1>,<t2>,...   clack: open just before each hit, the clunk lands ON it
// This side rolls the dice (rare), keeps moments apart, times them from the NOW
// line, and owns every safety cutoff (relays off at boot, max on-time, off on any
// new clip / narrator / lost card link).
//
// Console (USB or HA raw cmd), all start with SKEL:
//   SKEL                    status            SKEL ON | OFF        enable (saved)
//   SKEL L|R|B [ms]         pulse (800 ms)    SKEL CLACK [n]       n clunks, 0.6 s apart
//   SKEL STOP               all off now       SKEL NEXT            next clip with cues WILL get its best moment
//   SKEL ALWAYS ON|OFF      every clip, best moment (bench, until reboot)
//   SKEL LAG|FALL|OPEN|DJ|SHOW|GAP <n>   tune (saved)   SKEL POL HIGH|LOW   relay trigger level (saved)
// =============================================================================

#include <Arduino.h>

static const int SKEL_PIN_L = 25;
static const int SKEL_PIN_R = 26;

void   skel_begin();                               // call FIRST in setup(): relays off
void   skel_tick();
void   skel_on_now(int n);                         // card started clip n (timing anchor)
void   skel_on_cues(int n, const char* groups);    // that clip's candidate moments
void   skel_stop(const char* why);                 // both off, drop anything scheduled
void   skel_set_narrator(bool speaking);           // narrators own the floor: no moments
bool   skel_console(const char* line);             // handles "SKEL ..." lines, false otherwise
bool   skel_enabled();
void   skel_set_enabled(bool on);
const String& skel_last();                         // last moment / decision, for HA
uint32_t skel_version();                           // bumps when skel_last() or enabled changes
