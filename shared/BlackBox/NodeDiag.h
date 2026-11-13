#pragma once
// =============================================================================
// NodeDiag.h — small diagnostics every YX5300 prop shares (2026-10-01, from the
// Spooky Tree's silent-at-night hunt). Header-only; include in ONE .cpp (main).
//
//   SerialLineReader  noise-proof USB serial command input. UART0 RX floats
//                     with no USB host attached and picks up noise. A command
//                     is printable ASCII <= 64 chars; any line holding another
//                     byte is dropped whole and its bytes counted (junk = a free
//                     noise meter for the per-minute log line).
//   RxFrameLog        logs each ESP-NOW frame a node acts on once per change of
//                     (command, sender) -> (session, payload_a); repeats counted.
//   LoopGap           longest gap between loop() starts; WARN at >= 5 s.
// =============================================================================
#include <Arduino.h>
#include "BlackBox.h"
#include "ProjectConstants.h"

class SerialLineReader {
public:
    // true = a clean command line is ready in `out` (trimmed, maybe empty).
    bool poll(String& out) {
        while (Serial.available()) {
            const char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                const bool bad = _bad;
                _bad = false;
                if (bad) { _line = ""; continue; }      // noise: dropped, already counted
                out = _line; out.trim(); _line = "";
                if (out.length()) Log.echoFor(1800000);   // someone is at the bench: echo 30 min
                return true;
            }
            if (c >= 0x20 && c <= 0x7E && _line.length() < 64) _line += c;
            else { _bad = true; junk++; junkMin++; }
        }
        return false;
    }
    // "unknown command" logs at most once per 10 s, so noise can't flood the log.
    void unknown(const String& line) {
        if (!line.length() || millis() - _unknownAt < 10000) return;
        _unknownAt = millis();
        logTs(); Log.printf("[bench] unknown command '%s'\n", line.c_str());
    }
    uint32_t junk = 0, junkMin = 0;     // noise bytes since boot / since the last status line
private:
    String   _line;
    bool     _bad = false;
    uint32_t _unknownAt = 0;
};

class RxFrameLog {
public:
    void log(const Msg& msg) {
        for (uint8_t i = 0; i < _n; i++) {
            Last& l = _last[i];
            if (l.cmd != msg.command || l.sender != msg.sender) continue;
            if (l.session == msg.session_id && l.a == msg.payload_a) { repeats++; return; }
            l.session = msg.session_id; l.a = msg.payload_a;
            print(msg, "");
            return;
        }
        Last& l = _n < kSlots ? _last[_n++] : _last[_next++ % kSlots];
        l = { msg.command, msg.sender, msg.session_id, msg.payload_a };
        print(msg, " (first)");
    }
    uint32_t repeats = 0;
private:
    static void print(const Msg& m, const char* tag) {
        logTs(); Log.printf("[rx] cmd=0x%02X from=0x%02X session=0x%08lX a=%lu b=%lu%s\n", m.command, m.sender,
                            (unsigned long)m.session_id, (unsigned long)m.payload_a, (unsigned long)m.payload_b, tag);
    }
    struct Last { uint8_t cmd, sender; uint32_t session, a; };
    static constexpr uint8_t kSlots = 16;
    Last    _last[kSlots] = {};
    uint8_t _n = 0, _next = 0;
};

class LoopGap {
public:
    // Call first thing in loop(). Returns the gap since the previous loop() start.
    uint32_t tick(uint32_t now, uint8_t state, uint8_t track) {
        const uint32_t gap = _prev ? now - _prev : 0;
        _prev = now;
        if (gap > maxMs) maxMs = gap;
        if (gap >= 5000) {
            slow++;
            logTs(); Log.printf("[main] WARN loop blocked %lu ms (state=%u track=%u)\n",
                                (unsigned long)gap, (unsigned)state, (unsigned)track);
        }
        return gap;
    }
    uint32_t maxMs = 0;   // this status period (the status line resets it)
    uint32_t slow  = 0;   // gaps >= 5 s since boot
private:
    uint32_t _prev = 0;
};
