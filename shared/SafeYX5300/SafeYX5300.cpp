#include "SafeYX5300.h"

// Where [mp3] lines go (default USB serial; the Spooky Tree points it at its
// black-box log, 2026-10-01).
static Print* s_log = &Serial;
void SafeYX5300::setLog(Print& out) { s_log = &out; }

static void mp3Ts() {
    uint32_t ms = millis();
    s_log->printf("[%02u:%02u.%03u] [mp3] ", ms / 60000, (ms % 60000) / 1000, ms % 1000);
}

const char* SafeYX5300::endName(Mp3End e) {
    switch (e) {
        case Mp3End::FILE_END:          return "FILE_END";
        case Mp3End::LENGTH_REACHED:    return "LENGTH_REACHED";
        case Mp3End::STOPPED_BY_MODULE: return "STOPPED_BY_MODULE";
        case Mp3End::START_FAILED:      return "START_FAILED";
        case Mp3End::FILE_ERROR:        return "FILE_ERROR";
        case Mp3End::MODULE_RESET:      return "MODULE_RESET";
        default:                        return "NONE";
    }
}

// ── P1 pacing ────────────────────────────────────────────────────────────────
void SafeYX5300::pace() {
    while (millis() - _lastCmd < _cfg.paceMs) {
        while (_mp3.check()) handle(_mp3.getStatus());   // keep the RX side drained while waiting
        delay(1);
    }
    _lastCmd = millis();
}

void SafeYX5300::drain(uint32_t ms) { pump(millis() + ms); }

void SafeYX5300::pump(uint32_t untilMs) {
    while ((int32_t)(untilMs - millis()) > 0) {
        while (_mp3.check()) handle(_mp3.getStatus());
        delay(1);   // yield: keeps the IDF task watchdog fed
    }
}

// ── P2 warm handshake ────────────────────────────────────────────────────────
void SafeYX5300::selectTf() {
    pace(); _mp3.device(2); delay(_cfg.tfSettleMs);      // device(2) must go TWICE
    pace(); _mp3.device(2); delay(_cfg.tfSettleMs);
}

void SafeYX5300::warmHandshake(const char* why) {
    mp3Ts(); s_log->printf("warm handshake (%s)\n", why);
    _inHandshake = true;
    pace(); _mp3.playStop();
    pace(); _mp3.reset();
    delay(_cfg.resetDwellMs);
    selectTf();
    pace(); _mp3.repeat(false);
    pace(); _mp3.shuffle(false);
    pace(); _mp3.equalizer(_cfg.equalizer);
    pace(); _mp3.volume(_cfg.volume);
    drain(_cfg.drainMs);                                 // flush the post-RESET status bytes
    pace(); _mp3.playStop();                             // defuse boot auto-play
    drain(100);
    _inHandshake   = false;
    _needHandshake = false;
    _lastHandshakeAt = millis();
}

void SafeYX5300::quiet(uint8_t rxPin, uint8_t txPin, uint32_t baud) {
    if (!_portUp) {
        _port.begin(baud, SERIAL_8N1, rxPin, txPin);
        _mp3.begin();
        _mp3.setSynchronous(false);
        _portUp = true;
    }
    _mp3.playStop();
    while (_mp3.check()) {}                              // discard whatever it said; begin() starts clean
}

void SafeYX5300::begin(const SafeYX5300Config& cfg) {
    _cfg = cfg;
    if (!_portUp) { _port.begin(_cfg.baud, SERIAL_8N1, _cfg.rxPin, _cfg.txPin); _mp3.begin(); _portUp = true; }
    while (_port.available()) _port.read();              // stale bytes from quiet() / power-up noise
    _mp3.setSynchronous(false);
    mp3Ts(); s_log->printf("UART RX=%u TX=%u @%lu, pace %u ms\n", _cfg.rxPin, _cfg.txPin,
                           (unsigned long)_cfg.baud, _cfg.paceMs);
    delay(_cfg.bootDelayMs);
    _lastCmd = millis();
    warmHandshake("boot");
    _track = 0; _finished = false; _endReason = Mp3End::NONE;
    _lastPoll = millis(); _beganAt = millis();
    mp3Ts(); s_log->println(_lastRx ? "init complete (module answering)"
                          : _stats.echoes ? "WARN init complete but only our own commands came back (ECHO) — module not answering"
                                          : "WARN init complete but NO bytes from module — check 5V/GND/TX->16/RX<-17");
}

// ── status dispatch (every byte from the module comes through here) ──────────
void SafeYX5300::handle(const MD_YX5300::cbData* s) {
    if (!s) return;
    const uint32_t now = millis();
    switch (s->code) {
        case MD_YX5300::STS_TIMEOUT:
        case MD_YX5300::STS_VERSION:
        case MD_YX5300::STS_CHECKSUM:
            return;                                      // library-generated, not from the module
        default: break;
    }
    if (_rawN < kRawRx) _raw[_rawN++] = { (uint8_t)s->code, (uint16_t)s->data, now };
    else { memmove(_raw, _raw + 1, sizeof(RawRx) * (kRawRx - 1)); _raw[kRawRx - 1] = { (uint8_t)s->code, (uint16_t)s->data, now }; }
    // 0x04-0x39 are COMMAND codes (play, volume, select device, ...): the module
    // never sends them. Receiving one = the ESP is hearing its own TX on its RX
    // (Tree 2026-10-26: TX/RX bridged or the module not driving its TX line).
    // Not a sign of life: _lastRx is NOT moved, so the silent-module rule (P7)
    // still fires. Our own queryStatus echoes back as a fake "status 0x0000"
    // too, so while echoes are seen a status reply is not trusted either.
    if (s->code >= 0x04 && s->code <= 0x39) {
        _stats.echoes++; _lastEcho = now;
        if (!_lastEchoWarn || now - _lastEchoWarn > 60000UL) {
            _lastEchoWarn = now;
            mp3Ts(); s_log->printf("WARN ECHO: got our own command 0x%02X back — the module is NOT answering; "
                                   "TX/RX wires bridged (wet/shorted connector?) or the module has no power\n",
                                   (unsigned)s->code);
        }
        return;
    }
    const bool echoing = _lastEcho && now - _lastEcho < 10000UL;
    if ((s->code == MD_YX5300::STS_STATUS || s->code == MD_YX5300::STS_TOT_FILES) && echoing) return;   // probably our own query
    _lastRx = now;
    switch (s->code) {
        case MD_YX5300::STS_STATUS:
            _lastState = s->data & 0xFF; _stateFresh = true;
            _lastStatusRaw = s->data;
            break;
        case MD_YX5300::STS_TOT_FILES:
            _sdFiles = s->data;
            break;
        case MD_YX5300::STS_FILE_END:
            if (!_track) break;                          // nothing of ours was playing
            if (_looping) {
                // 2026-10-01 (Tree black box): the module does NOT reliably repeat
                // a CMD 0x08 track. It played 001 once, then sat stopped until the
                // 30 s status poll noticed (~40 s of silence every 7 min). So on
                // every wrap we replay it ourselves (tick(), next pass): ~0.5 s
                // gap. If the module did repeat, this just restarts it from 0.
                if (now - _playCmdAt < _cfg.staleEndMs) { _stats.staleEnds++; break; }   // P8
                _stats.loopWraps++;
                _loopReplay = true;
                break;
            }
            if (now - _playCmdAt < _cfg.staleEndMs) {    // P8
                _stats.staleEnds++;
                mp3Ts(); s_log->printf("FILE_END %u right after PLAY — stale, ignored\n", (unsigned)s->data);
                break;
            }
            if (s->data && s->data != _track) {
                mp3Ts(); s_log->printf("FILE_END index %u != playing %u (module quirk, logged)\n",
                                       (unsigned)s->data, (unsigned)_track);
            }
            endTrack(Mp3End::FILE_END, true);            // P4: stop it before it advances
            break;
        case MD_YX5300::STS_PLAYING:
            if (_track && s->data && s->data != _track) {
                _stats.indexMismatch++;
                mp3Ts(); s_log->printf("WARN module reports file %u, expected %u\n",
                                       (unsigned)s->data, (unsigned)_track);
            }
            break;
        case MD_YX5300::STS_INIT:                        // P7: module rebooted by itself
            if (_inHandshake) break;                     // our own RESET's INIT, not a power dip
            _stats.moduleResets++;
            mp3Ts(); s_log->println("WARN unsolicited INIT — module reset (power dip?)");
            _needHandshake = true;
            if (_track) { _track = 0; _finished = true; _endReason = Mp3End::MODULE_RESET; }
            break;
        case MD_YX5300::STS_ERR_FILE: {
            // 0x40 is the module's generic error reply; the data byte says which
            // (MD_YX5300 calls it "file not found" for all of them). Only 5/6/8
            // are about the file. 1 busy, 2 sleep, 3 serial frame, 4 checksum are
            // transient: log + count, never end the track (bench 2026-09-26: a
            // data=3 killed a Corn theme 1:18 in). P6 poll still catches a real stop.
            const uint8_t e = s->data & 0xFF;
            if (e == 5 || e == 6 || e == 8) {
                _stats.fileErrors++;
                mp3Ts(); s_log->printf("ERROR file not found / unreadable (err=%u, track=%u) — check SD layout\n",
                                       (unsigned)e, (unsigned)_track);
                if (_track) { _track = 0; _finished = true; _endReason = Mp3End::FILE_ERROR; }
            } else {
                _stats.moduleErrors++;
                mp3Ts(); s_log->printf("WARN module error reply err=%u (%s) — ignored, track %u continues\n",
                                       (unsigned)e, e == 1 ? "busy" : e == 2 ? "sleep" : e == 3 ? "serial frame" :
                                       e == 4 ? "checksum" : "other", (unsigned)_track);
            }
            break;
        }
        case MD_YX5300::STS_TF_REMOVE:
            mp3Ts(); s_log->println("WARN SD card removed");
            break;
        case MD_YX5300::STS_TF_INSERT:
            mp3Ts(); s_log->println("SD card inserted — re-handshake");
            _needHandshake = true;
            break;
        default: break;                                  // ACK_OK, VOLUME, EQ ...
    }
}

bool SafeYX5300::waitStatus(uint32_t ms, int* stateOut) {
    _stateFresh = false;
    pace(); _mp3.queryStatus();
    uint32_t until = millis() + ms;
    while ((int32_t)(until - millis()) > 0) {
        while (_mp3.check()) handle(_mp3.getStatus());
        if (_stateFresh) { if (stateOut) *stateOut = _lastState; return true; }
        delay(1);
    }
    return false;
}

void SafeYX5300::endTrack(Mp3End why, bool sendStop) {
    uint8_t t = _track;
    uint32_t el = elapsedMs();
    _track = 0; _finished = true; _endReason = why;
    mp3Ts(); s_log->printf("track %u ended: %s after %lu.%lus\n", (unsigned)t, endName(why),
                           (unsigned long)(el / 1000), (unsigned long)((el % 1000) / 100));
    if (sendStop) { pace(); _mp3.playStop(); _lastStop = millis(); }
}

// Retry 3 of a start (2026-10-26, Tree: module answered but never reported
// "playing" through every warm handshake). Wake it in case it fell into sleep
// mode, give RESET 1.5 s instead of 0.6 s for a slow SD mount, then the normal
// handshake, then ask how many files it can see (0 / no answer = card not read).
void SafeYX5300::deepHandshake(const char* why) {
    mp3Ts(); s_log->printf("deep handshake (%s): wake-up, long RESET dwell, SD file count\n", why);
    _inHandshake = true;
    pace(); _mp3.wakeUp();
    drain(200);
    const uint16_t dwell = _cfg.resetDwellMs;
    _cfg.resetDwellMs = 1500;
    warmHandshake(why);                                  // clears _inHandshake
    _cfg.resetDwellMs = dwell;
    _sdFiles = -1;
    pace(); _mp3.queryFilesCount();
    drain(700);
    mp3Ts();
    if (_sdFiles < 0)       s_log->println("WARN SD file count: no answer (card not mounted / not read?)");
    else if (_sdFiles == 0) s_log->println("WARN SD file count: 0 (card empty or unreadable)");
    else                    s_log->printf("SD file count: %ld\n", (long)_sdFiles);
}

void SafeYX5300::printReplies(const char* what) {
    mp3Ts(); s_log->printf("%s: %u raw replies in the last %lus:", what, (unsigned)_rawN,
                           (unsigned long)((millis() - _rawSince) / 1000));
    for (uint8_t i = 0; i < _rawN; i++)
        s_log->printf(" %02X/%04X@%lu", (unsigned)_raw[i].code, (unsigned)_raw[i].data,
                      (unsigned long)(_raw[i].ms - _rawSince));
    s_log->println();
    mp3Ts();
    if (_lastStatusRaw < 0) s_log->println("last status: never answered a status query");
    else s_log->printf("last status 0x%04lX: device=0x%02lX state=%ld (0 stop, 1 play, 2 pause)  SD files=%ld\n",
                       (unsigned long)_lastStatusRaw, (unsigned long)((_lastStatusRaw >> 8) & 0xFF),
                       (long)(_lastStatusRaw & 0xFF), (long)_sdFiles);
}

void SafeYX5300::probe() {
    _rawN = 0; _rawSince = millis();
    mp3Ts(); s_log->println("probe: status, file count, folder count, volume, playing file");
    pace(); _mp3.queryStatus();       drain(600);
    pace(); _mp3.queryFilesCount();   drain(600);
    pace(); _mp3.queryFolderCount();  drain(600);
    pace(); _mp3.queryVolume();       drain(600);
    pace(); _mp3.queryFile();         drain(600);
    printReplies("probe");
}

// ── P3 confirmed start ───────────────────────────────────────────────────────
bool SafeYX5300::play(uint8_t track, uint32_t lengthMs, bool loop) {
    if (_needHandshake) warmHandshake("pending");
    _finished = false; _endReason = Mp3End::NONE;
    _rawN = 0; _rawSince = millis();

    for (uint8_t attempt = 1; attempt <= _cfg.startAttempts; attempt++) {
        if (attempt == 2) { _stats.startRetries++; mp3Ts(); s_log->println("start not confirmed — re-select TF, retry"); selectTf(); }
        if (attempt >= 3) { _stats.startRetries++; deepHandshake("start not confirmed"); }
        else if (_needHandshake) warmHandshake("module reset during start");

        pace(); _mp3.playStop();                         // fast path STOP -> PLAY
        drain(80);
        pace(); if (loop) _mp3.playTrackRepeat(track); else _mp3.playTrack(track);
        _playCmdAt = millis();
        _track = track; _startedAt = _playCmdAt; _lengthMs = loop ? 0 : lengthMs; _looping = loop; _loopReplay = false;
        _loopLenMs = loop ? lengthMs : 0;
        _finished = false; _endReason = Mp3End::NONE;

        int st = -1;
        uint32_t until = millis() + _cfg.confirmMs;
        bool confirmed = false;
        while ((int32_t)(until - millis()) > 0 && _track == track) {
            if (waitStatus(400, &st) && st == 1) { confirmed = true; break; }
        }
        if (_endReason == Mp3End::FILE_ERROR) break;     // no point retrying a missing file
        if (confirmed) {
            _stats.plays++;
            _lastPoll = millis(); _pollFileNext = true;
            mp3Ts(); s_log->printf("PLAY track %u confirmed (attempt %u, %s%lus)\n",
                                   (unsigned)track, (unsigned)attempt, loop ? "LOOP, " : "length ",
                                   (unsigned long)(loop ? 0 : lengthMs / 1000));
            return true;
        }
    }
    _stats.startFails++;
    if (_endReason != Mp3End::FILE_ERROR) {
        mp3Ts(); s_log->printf("ERROR track %u never confirmed playing after %u attempts\n",
                               (unsigned)track, (unsigned)_cfg.startAttempts);
        printReplies("start failed");
        _endReason = Mp3End::START_FAILED;
    }
    pace(); _mp3.playStop();
    _track = 0; _finished = true;
    return false;
}

// ── P5 verified stop ─────────────────────────────────────────────────────────
void SafeYX5300::stop(const char* why) {
    const bool wasOurs = _track != 0;
    _track = 0; _finished = false; _endReason = Mp3End::NONE;
    if (millis() - _lastStop < 300) return;              // debounce (safeStop)
    mp3Ts(); s_log->printf("STOP (%s)\n", why);
    for (uint8_t i = 0; i < _cfg.stopAttempts; i++) {
        if (i) { _stats.stopResends++; mp3Ts(); s_log->println("still playing — STOP again"); }
        pace(); _mp3.playStop();
        _lastStop = millis();
        int st = -1;
        if (!waitStatus(_cfg.stopVerifyMs, &st)) {      // no answer: nothing to verify against
            if (wasOurs) { mp3Ts(); s_log->println("WARN no status reply after STOP"); }
            return;
        }
        if (st != 1) return;                             // stopped / paused
    }
    mp3Ts(); s_log->println("ERROR module still playing after repeated STOP");
}

// ── tick: P4 length stop, P6 poll/defuse, P7 recovery ────────────────────────
void SafeYX5300::tick() {
    while (_mp3.check()) handle(_mp3.getStatus());
    const uint32_t now = millis();

    if (_needHandshake) {
        warmHandshake("module reset / SD re-insert");
        return;
    }

    if (_loopReplay) {                                   // loop track wrapped: replay it now
        _loopReplay = false;
        if (_track && _looping) {
            // P10 (Joe 2026-10-26: "these players need cleaning out or they get
            // stuck"): at a wrap — the one moment a loop has no audio anyway —
            // a routine STOP + RESET + TF re-select every loopCleanMs, then replay.
            if (_cfg.loopCleanMs && millis() - _lastHandshakeAt >= _cfg.loopCleanMs) {
                _stats.loopCleans++;
                warmHandshake("routine clean at loop wrap");
            }
            pace(); _mp3.playTrackRepeat(_track);
            _playCmdAt = millis(); _lastPoll = _playCmdAt;   // next status poll a full period later
            mp3Ts(); s_log->printf("loop track %u wrapped (%u) — replayed\n", (unsigned)_track, (unsigned)_stats.loopWraps);
        }
    }

    // P9 (2026-10-26): a loop of known length must wrap (FILE_END -> replay above
    // moves _playCmdAt) by length + loopOverdueMs. If not, the module is stuck
    // mid-file even if it still answers "playing": clean re-handshake + restart.
    if (_track && _looping && _loopLenMs && (int32_t)(now - _playCmdAt) > (int32_t)(_loopLenMs + _cfg.loopOverdueMs)) {
        _stats.loopOverdue++;
        mp3Ts(); s_log->printf("WARN loop track %u overdue: no wrap %lus after (re)play (length %lus) — stuck; "
                               "re-handshake + restart\n", (unsigned)_track, (unsigned long)((now - _playCmdAt) / 1000),
                               (unsigned long)(_loopLenMs / 1000));
        _track = 0; _finished = true; _endReason = Mp3End::MODULE_RESET;
        _needHandshake = true;
        return;
    }

    if (_track && _lengthMs && now - _startedAt >= _lengthMs + _cfg.endGraceMs) {  // P4
        _stats.lengthStops++;
        endTrack(Mp3End::LENGTH_REACHED, true);
        return;
    }

    if (now - _lastPoll >= _cfg.pollMs) {                                          // P6
        _lastPoll = now;
        if (_track && _pollFileNext) {                   // alternate: verify which file is playing
            _pollFileNext = false;
            pace(); _mp3.queryFile();
        } else {
            _pollFileNext = true;
            int st = -1;
            if (waitStatus(600, &st)) {
                if (!_track && st == 1 && millis() - _lastStop > 1000) {
                    _stats.defuses++;
                    mp3Ts(); s_log->println("WARN module playing while idle — defuse STOP");
                    pace(); _mp3.playStop(); _lastStop = millis();
                } else if (_track && st == 0 && millis() - _playCmdAt > 3000) {
                    _stats.moduleStops++;
                    endTrack(Mp3End::STOPPED_BY_MODULE, false);   // missed FILE_END
                }
            }
        }
    }

    // Fresh clock: the P6 poll above can receive a reply (and move _lastRx) AFTER
    // `now` was taken, and `now - _lastRx` then wrapped to ~4294967 s -> a false
    // "silent" re-handshake (STOP + RESET) that killed every theme ~30-75 s in
    // (bench 2026-09-26, Pumpkin Men). Signed compare so a future stamp never trips it.
    const uint32_t nowS    = millis();
    const uint32_t heardAt = _lastRx ? _lastRx : _beganAt;   // never answered counts from boot
    if (_beganAt && (int32_t)(nowS - heardAt) > (int32_t)_cfg.silentModuleMs &&     // P7
        nowS - _lastSilentWarn > _cfg.silentModuleMs) {
        _lastSilentWarn = nowS;
        _stats.silentWarns++;
        if (_track && _looping) {
            // A loop (idle/ambient bed) is ALWAYS "mid-track", so the rule below
            // meant a module that died during the ambient was never recovered by
            // the driver — only the node's 7 min watchdog (tree 2026-09-29: "not
            // playing, no matter how many power cycles"). An ambient restart costs
            // nothing: end it as MODULE_RESET (the node restarts it at once) and
            // re-handshake first. A real show track is still never cut.
            _stats.loopRecovers++;
            mp3Ts(); s_log->printf("WARN no bytes from module for %lus during loop track %u — re-handshake + restart it\n",
                                   (unsigned long)((nowS - heardAt) / 1000), (unsigned)_track);
            _track = 0; _finished = true; _endReason = Mp3End::MODULE_RESET;
            _needHandshake = true;
        } else if (_track) {
            // Mid-track silence is NOT proof the audio stopped (bench 2026-09-26:
            // the module went quiet during theme runs while playing fine, likely
            // motor/relay noise on the UART). A re-handshake sends STOP + RESET
            // and would cut the show: warn only; the length stop still ends it,
            // and the next idle moment re-handshakes if it is still silent.
            mp3Ts(); s_log->printf("WARN no bytes from module for %lus while track %u plays — NOT resetting mid-track\n",
                                   (unsigned long)((nowS - heardAt) / 1000), (unsigned)_track);
        } else {
            mp3Ts(); s_log->printf("WARN no bytes from module for %lus — check 5V/wiring; re-handshake\n",
                                   (unsigned long)((nowS - heardAt) / 1000));
            _needHandshake = true;
        }
    }
}
