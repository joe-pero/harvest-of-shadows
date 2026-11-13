#pragma once

#include <Arduino.h>
#include "SafeYX5300.h"

// Patch MP3 layer — thin wrapper over shared/SafeYX5300 (2026-10-01, replaces the
// node's own 09-06 mp3.cpp). All pacing / handshake / confirmed start / verified
// stop / defuse / module recovery / loop replay lives in the shared driver.

// ── Hardware (matches the Spooky Tree) ─────────────────────────────────────────
#define MP3_RX_PIN   16     // ESP32 RX  <- YX5300 TX
#define MP3_TX_PIN   17     // ESP32 TX  -> YX5300 RX

// ── SD card track layout — DEDICATED card per patch side ───────────────────────
//   001.mp3  idle-ambient bed for THIS channel (loops whenever the show is idle)
//   002.mp3  CORN theme soundscape — THIS channel
//   003.mp3  ROTLD theme soundscape — THIS channel
//   004.mp3  MUTANT PUMPKINS theme soundscape — THIS channel
// Same numbers both sides — the firmware never needs to know which side it is.
#define TRACK_AMBIENT         1
#define TRACK_THEME_CORN      2
#define TRACK_THEME_ROTLD     3
#define TRACK_THEME_PUMPKINS  4

// Theme mixes: house rule = every theme track 3:00 (Joe 2026-09-24). The Patch
// mixes don't exist yet (2026-10-01); change this if they come out different.
// The driver STOPs at length + 2 s even if FILE_END is missed.
#define LEN_THEME_MS        180000UL

void     mp3Init();                        // blocking warm handshake; call once from setup()
void     mp3Tick();                        // call every loop()
bool     mp3PlayTrack(uint8_t track);      // confirmed start; false = never confirmed / file error
bool     mp3PlayLoop(uint8_t track);       // same, replayed on every wrap (the ambient bed)
void     mp3Stop(const char* why = "stop");
bool     mp3IsFinished();                  // track ended (FILE_END / length / module stop / failure)
void     mp3ClearFinished();
Mp3End   mp3EndReason();
uint8_t  mp3CurrentTrack();                // 0 when nothing of ours is playing
uint32_t mp3GetLastActivityMs();           // last byte received from the module
const SafeYX5300Stats& mp3Stats();
