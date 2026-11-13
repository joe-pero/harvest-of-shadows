#pragma once

#include <Arduino.h>
#include "SafeYX5300.h"

// Spooky Tree MP3 layer — thin wrapper over shared/SafeYX5300 (2026-09-24).
// All pacing / handshake / confirmed start / verified stop / defuse / module
// recovery lives in the shared driver (see its header for the rules and the
// 2025 sources they came from). This file only knows the tree's SD layout and
// how long each track is.

// ── Hardware ──────────────────────────────────────────────────────────────────
#define MP3_RX_PIN       16
#define MP3_TX_PIN       17

// ── Track layout on SD card ───────────────────────────────────────────────────
#define TRACK_AMBIENT       1     // tree ambient loop ("JackAttack", 6:21)
#define TRACK_ACTION_MIN    2     // PIR action tracks 2..6 (002 = elk scream)
#define TRACK_ACTION_MAX    6
#define TRACK_THEME_CORN    7     // show theme tracks — played on the Scarecrow's SHOW_ACTIVE
#define TRACK_THEME_ROTLD   8
#define TRACK_THEME_MPUMP   9

// ── Known track lengths (the module plays until told to stop: the driver
//    STOPs at length + grace even if FILE_END is missed) ──────────────────────
#define LEN_AMBIENT_MS      381000UL   // 001.mp3 measured 6:21.0; loops on the module, arms the driver's P9 overdue check
#define MP3_LOOP_CLEAN_MS   (30UL * 60UL * 1000UL)   // Joe 10-26: clean the module out every 30 min (at an ambient wrap)
#define LEN_ACTION_MAX_MS    60000UL   // action tracks: 1 min cap (unchanged from before)
#define LEN_THEME_MS        180000UL   // every theme track is trimmed to exactly 3:00 (Joe)

void     mp3Quiet();                       // STOP only, safe before mp3Init (silences power-up auto-play)
void     mp3Init();                        // blocking warm handshake; call once from setup()
void     mp3Tick();                        // call every loop()
bool     mp3PlayTrack(uint8_t track);      // confirmed start; false = never confirmed / file error
bool     mp3PlayLoop(uint8_t track);       // same, gapless module-side repeat (the ambient)
void     mp3Stop(const char* why = "stop");
bool     mp3IsFinished();                  // track ended (FILE_END / length / module stop / failure)
void     mp3ClearFinished();
Mp3End   mp3EndReason();
uint8_t  mp3CurrentTrack();                // 0 when nothing of ours is playing
uint32_t mp3GetLastActivityMs();           // last byte received from the module (status poll keeps it fresh)
const SafeYX5300Stats& mp3Stats();
bool     mp3Echoing();                     // our own commands came back on RX in the last 60 s
void     mp3Probe();                       // bench `mp3probe`: raw module answers (status, SD files, ...)
