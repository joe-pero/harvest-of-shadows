#pragma once

#include <Arduino.h>
#include "SafeYX5300.h"

// Pumpkin Men MP3 layer — thin wrapper over shared/SafeYX5300 (2026-09-24).
// Pacing / handshake / confirmed start / verified stop / defuse / module
// recovery all live in the shared driver (rules + 2025 sources in its header).

// ── Hardware ──────────────────────────────────────────────────────────────────
// Same YX5300 wiring as Spooky Tree and the 2025 Master Brain box.
#define MP3_RX_PIN       16
#define MP3_TX_PIN       17

// ── Track layout on SD card (rebuilt 2026-10-07, 19 tracks) ─────────────────
// Copy order matters on the YX5300 (FAT directory order): the card is written
// blank, one file at a time, 001 -> 019, from the staged folder
// ~/Desktop/Spatial audio/SD cards (ready to copy)/Pumpkin Heads (near the barn)/
// (README there lists each file's source).
//   001 = Children of the Corn (3:00)     002 = Return of the Living Dead (3:00)
//   003 = Mutant Pumpkins ("New mutant.mp3", 3:00, Joe 2026-10-07). Replaces the
//         09-24 placeholder and the 2:09 action scene (old 004), both removed.
//   004..018 = short action clips (Old action/002..016, 4–11 s)
//   019 = idle loop (gapless module-side repeat, no effects)
#define TRACK_THEME_CORN    1
#define TRACK_THEME_ROTLD   2
#define TRACK_THEME_MPUMP   3
#define TRACK_CLIP_FIRST    4
#define TRACK_CLIP_LAST     18
#define TRACK_IDLE_LOOP    19   // plays on loop whenever no theme/action plays

// ── Known track lengths (the module plays until told to stop: the driver STOPs
//    at length + grace even if FILE_END is missed) ────────────────────────────
#define LEN_THEME_MS         180000UL  // every theme track is 3:00 (Joe; New mutant = 3:00.06)
// Short clips 004..018, measured with ffprobe 2026-09-26 (source Old action/002..016).
// Was one 11 s cap for all: a 6.4 s clip kept the jaws going ~6.6 s in silence.
static const uint32_t LEN_CLIP_MS[TRACK_CLIP_LAST - TRACK_CLIP_FIRST + 1] = {
    6400, 4650, 7367, 4571, 6766, 9169, 4075, 8856,      // 004..011
    8568, 9064, 5355, 10867, 5355, 7863, 7367 };         // 012..018

uint32_t mp3TrackLengthMs(uint8_t track);   // known audio length, 0 = unknown
void     mp3Init();                        // blocking warm handshake; call once from setup()
void     mp3Tick();                        // call every loop()
bool     mp3PlayTrack(uint8_t track);      // confirmed start; false = never confirmed / file error
bool     mp3PlayLoop(uint8_t track);       // same, but the module repeats it gaplessly
bool     mp3IsLooping();
void     mp3Stop(const char* why = "stop");
bool     mp3IsFinished();
void     mp3ClearFinished();
Mp3End   mp3EndReason();
uint8_t  mp3CurrentTrack();
uint32_t mp3GetLastActivityMs();           // last byte received from the module
const SafeYX5300Stats& mp3Stats();
