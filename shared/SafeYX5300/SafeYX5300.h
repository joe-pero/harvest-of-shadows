// =============================================================================
// SafeYX5300 — hardened YX5300 (Catalex serial MP3) driver, shared by the
// theme-bed props (Spooky Tree, Pumpkin Men; Patch L/R when built).
// =============================================================================
// Written 2026-09-24 from Joe's own 2025 code + notes, not from the datasheet:
//   Halloween-Dev/.../Master Halloween Show 2026/docs/harvest_notes/
//     mp3_serial_findings.md, hardware_quirks.md
//   Projects/Scarecrow/src/Scarecrow.ino (mp3StartReliable, safeStop, defuse,
//     drainUART), Projects/Scarecrow/src/yx5300_player.cpp,
//   Projects/Talking Skeleton/src/ts_mp3.cpp, Projects/Master (zones.cpp)
//
// What it guarantees, and where each rule came from:
//   P1 Pacing: every command waits until paceMs after the previous one.
//      500 ms default ("use 500 ms for any new node", hardware_quirks.md).
//   P2 Warm handshake: STOP, RESET, dwell, device(2) TWICE with settle,
//      repeat off, shuffle off, EQ 5, volume 30, drain UART, STOP again
//      (defuses the boot auto-play). Same order as the 2025 nodes.
//   P3 Start is CONFIRMED: STOP -> drain -> PLAY -> queryStatus must report
//      "playing". Retry 2: re-select TF twice. Retry 3: DEEP handshake (wake-up,
//      RESET with a 1.5 s dwell, then the warm handshake + SD file count;
//      2026-10-26). (Scarecrow.ino mp3StartReliable.) Unconfirmed -> START_FAILED,
//      logged with every raw module reply seen during the attempts (Tree
//      2026-10-26: "answering" but never "playing", and nothing said why).
//   P4 The module plays until it is told to stop (Joe): on FILE_END we send
//      STOP straight away, and every track has a known length — at length +
//      grace the driver sends STOP itself even if FILE_END never came.
//   P5 Stop is VERIFIED: STOP, then queryStatus; still playing -> STOP again
//      (up to stopAttempts). Debounced like safeStop().
//   P6 Status poll every pollMs: idle but module says playing -> STOP
//      (defuse, Scarecrow.ino gDefuseStopPending); playing but module says
//      stopped -> end (missed FILE_END). queryFile index is logged/counted.
//   P7 Unsolicited INIT (module rebooted / brownout) -> re-handshake; a track
//      that was playing ends with MODULE_RESET. ERR_FILE -> FILE_ERROR.
//      (Only 0x40 err 5/6/8 = file; 1-4 are transient, logged.) TF_REMOVE logged. No bytes at all for silentModuleMs -> WARN (wiring /
//      5 V) + re-handshake, at most once per silentModuleMs — only when no track
//      plays (mid-track silence = warn only, never STOP+RESET a running show).
//      Exception: a LOOP track (idle bed) is ended as MODULE_RESET + re-handshake,
//      so the node restarts it (2026-09-29, tree silent until watchdog).
//   P8 FILE_END within staleEndMs of a PLAY is from the previous track: ignored.
//   P9 Loop tracks (idle beds): CMD 0x08 is sent, but the module was seen NOT to
//      repeat (Tree 2026-10-01: ~40 s silent every 7 min). Every FILE_END on a
//      loop track replays it from tick() (~0.5 s gap); the 30 s status poll
//      still catches a missed FILE_END (STOPPED_BY_MODULE -> node restarts it).
// Every decision is printed with a [mp3] prefix; stats() feeds the alive line.
// =============================================================================
#pragma once
#include <Arduino.h>
#include <MD_YX5300.h>

struct SafeYX5300Config {
    uint8_t  rxPin          = 16;
    uint8_t  txPin          = 17;
    uint32_t baud           = 9600;
    uint16_t paceMs         = 500;    // P1
    uint16_t bootDelayMs    = 2000;   // power-on dwell before the first command
    uint16_t resetDwellMs   = 600;    // after RESET (Scarecrow cold boot)
    uint16_t tfSettleMs     = 500;    // after each device(2)
    uint16_t drainMs        = 300;
    uint8_t  volume         = 30;
    uint8_t  equalizer      = 5;      // Bass, all 2025 nodes
    uint16_t confirmMs      = 2000;   // P3 per attempt
    uint8_t  startAttempts  = 3;
    uint8_t  stopAttempts   = 3;      // P5
    uint16_t stopVerifyMs   = 900;
    uint32_t pollMs         = 15000;  // P6
    uint32_t endGraceMs     = 2000;   // P4: STOP at known length + this
    uint16_t staleEndMs     = 1500;   // P8
    uint32_t silentModuleMs = 60000;  // P7
    uint32_t loopOverdueMs  = 60000;  // P9: loop with a known length that hasn't wrapped by length + this = stuck
                                      //     (Tree 10-26: 6:21 file wrapped 398 s after PLAY — module runs slow + replay gap)
    uint32_t loopCleanMs    = 0;      // P10: re-handshake (STOP+RESET+TF) at a loop wrap this often; 0 = off
};

enum class Mp3End : uint8_t {
    NONE, FILE_END, LENGTH_REACHED, STOPPED_BY_MODULE, START_FAILED, FILE_ERROR, MODULE_RESET
};

struct SafeYX5300Stats {
    uint16_t plays = 0, startRetries = 0, startFails = 0, stopResends = 0, defuses = 0;
    uint16_t lengthStops = 0, moduleStops = 0, staleEnds = 0, fileErrors = 0, moduleErrors = 0, loopWraps = 0;
    uint16_t moduleResets = 0, silentWarns = 0, indexMismatch = 0, loopRecovers = 0;
    uint16_t echoes = 0;                               // our own commands heard back on RX (2026-10-26)
    uint16_t loopOverdue = 0, loopCleans = 0;          // P9 / P10 (2026-10-26)
};

class SafeYX5300 {
public:
    explicit SafeYX5300(HardwareSerial& port) : _port(port), _mp3(port) {}

    void begin(const SafeYX5300Config& cfg);          // blocking: boot dwell + P2
    // Before begin(), during the node's power-on settle: STOP only, so a module
    // that auto-plays at its own power-up is silenced at once (Tree 2026-10-26:
    // its MP3 has its own supply, so it boots ~20 s before the ESP talks to it).
    void quiet(uint8_t rxPin, uint8_t txPin, uint32_t baud = 9600);

    // Play `track`; `lengthMs` = its known length (0 = unknown, no length stop).
    // Blocking up to ~startAttempts * (confirmMs + pacing). Returns confirmed.
    // loop = gapless repeat on the module itself (CMD 0x08 single-cycle play):
    // FILE_END at each wrap is counted, not treated as the end; no length stop.
    // A loop's lengthMs (if known) arms P9: no wrap by length + loopOverdueMs =
    // module stuck mid-file -> ended as MODULE_RESET with a re-handshake first.
    bool play(uint8_t track, uint32_t lengthMs, bool loop = false);
    bool looping() const { return _track && _looping; }
    void stop(const char* why);                        // P5, clears finished
    void tick();                                       // call every loop()

    bool     finished() const  { return _finished; }
    Mp3End   endReason() const { return _endReason; }
    void     clearFinished()   { _finished = false; _endReason = Mp3End::NONE; }
    bool     playing() const   { return _track != 0; }
    uint8_t  current() const   { return _track; }
    uint32_t elapsedMs() const { return _track ? millis() - _startedAt : 0; }
    uint32_t lastRxMs() const  { return _lastRx; }     // last byte-level status from the module
    bool     echoing() const   { return _lastEcho && millis() - _lastEcho < 60000UL; }   // own commands heard back, last 60 s
    const SafeYX5300Stats& stats() const { return _stats; }
    static const char* endName(Mp3End e);
    static void setLog(Print& out);                    // [mp3] output (default Serial)
    void probe();                                      // bench: query status/files/folders/volume, print raw replies

private:
    void pace();
    void warmHandshake(const char* why);
    void selectTf();
    void drain(uint32_t ms);
    void pump(uint32_t untilMs);                        // drain + dispatch until time
    void handle(const MD_YX5300::cbData* s);
    bool waitStatus(uint32_t ms, int* stateOut);        // queryStatus, wait for reply
    void endTrack(Mp3End why, bool sendStop);
    void deepHandshake(const char* why);
    void printReplies(const char* what);                // the raw-reply ring + decoded last status

    // Last raw replies from the module (code, data), cleared at each play().
    struct RawRx { uint8_t code; uint16_t data; uint32_t ms; };
    static constexpr uint8_t kRawRx = 12;
    RawRx    _raw[kRawRx];
    uint8_t  _rawN = 0;
    uint32_t _rawSince = 0;
    int32_t  _lastStatusRaw = -1;                        // full STS_STATUS data: hi = device, lo = state
    int32_t  _sdFiles = -1;
    uint32_t _lastEcho = 0, _lastEchoWarn = 0;                              // STS_TOT_FILES answer, -1 = never answered

    HardwareSerial&  _port;
    MD_YX5300        _mp3;
    SafeYX5300Config _cfg;
    SafeYX5300Stats  _stats;

    uint32_t _beganAt = 0, _lastCmd = 0, _lastRx = 0, _lastPoll = 0, _lastSilentWarn = 0, _lastStop = 0;
    uint32_t _startedAt = 0, _lengthMs = 0, _playCmdAt = 0;
    uint32_t _loopLenMs = 0, _lastHandshakeAt = 0;
    bool     _portUp = false;
    uint8_t  _track = 0;
    bool     _finished = false, _needHandshake = false, _pollFileNext = false;
    bool     _inHandshake = false;                     // swallow the INIT our own RESET causes
    bool     _looping = false;                         // current track is a module-side repeat
    bool     _loopReplay = false;                      // FILE_END on a loop track: replay it in tick()
    Mp3End   _endReason = Mp3End::NONE;
    int      _lastState = -1;                            // from STS_STATUS: 0 stop, 1 play, 2 pause
    bool     _stateFresh = false;
};
