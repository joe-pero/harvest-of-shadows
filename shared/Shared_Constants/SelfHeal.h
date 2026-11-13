#pragma once
// =============================================================================
// SelfHeal.h — when a YX5300 prop should reboot itself (2026-10-01, Joe: "I
// like the self-heal a lot, just don't let it hiccup the show in a strange way").
//
// Why: the Spooky Tree went silent at night and came back every time it was
// rebooted (a bridge connect reboots the DevKit). So a node that is clearly
// dead or flapping reboots itself, logged, instead of waiting for a person.
//
// Triggers (any one):
//   MODULE_SILENT  no byte from the MP3 module for moduleSilentMs (the driver
//                  already re-handshook at 60 s; this is the next step)
//   START_FAILS    the idle bed failed to start startFailsMax times in a row
//   LOOP_CHURN     the idle bed died and was restarted churnMax times within
//                  churnWindowMs (module keeps dropping it)
// When: only while the node says it is safe (idle: no theme run, no visit, no
// action track, no OTA). A trigger that arrives mid-show waits. Only
// MODULE_SILENT may override after deferMaxMs (the prop is already dead then).
// Never: while a person is at the bench (any serial command holds it
// benchHoldMs), when disabled (`heal off`), or after maxHealBoots self-heal
// reboots in a row (the node then just logs; a run of healthyMs clears it).
//
// Pure logic, no Arduino: host-tested in props/_tests/test_self_heal.cpp.
// =============================================================================
#include <stdint.h>

namespace selfheal {

enum class Why : uint8_t { NONE, MODULE_SILENT, START_FAILS, LOOP_CHURN };

inline const char* whyName(Why w) {
    switch (w) {
        case Why::MODULE_SILENT: return "MODULE_SILENT";
        case Why::START_FAILS:   return "START_FAILS";
        case Why::LOOP_CHURN:    return "LOOP_CHURN";
        default:                 return "NONE";
    }
}

struct Config {
    uint32_t moduleSilentMs = 120000;   // 2 min with no byte from the module
    uint8_t  startFailsMax  = 3;        // idle bed start failures in a row
    uint8_t  churnMax       = 4;        // idle bed deaths ...
    uint32_t churnWindowMs  = 600000;   // ... within 10 min
    uint32_t deferMaxMs     = 600000;   // MODULE_SILENT reboots even mid-show after this
    uint32_t benchHoldMs    = 600000;   // serial command -> no self-heal for 10 min
    uint8_t  maxHealBoots   = 3;        // self-heal reboots in a row, then stop
    uint32_t healthyMs      = 1800000;  // 30 min up -> the in-a-row count clears
};

struct Decision {
    Why  why      = Why::NONE;   // != NONE: reboot now
    bool forced   = false;       // rebooting although not safe (MODULE_SILENT past deferMaxMs)
};

class Monitor {
public:
    explicit Monitor(const Config& c = Config()) : _c(c) {}

    // Node events
    void idleStartOk()                { _startFails = 0; }
    void idleStartFailed()            { if (_startFails < 255) _startFails++; }
    void idleBedDied(uint32_t now) {
        _churn[_churnPos] = now ? now : 1;
        _churnPos = (uint8_t)((_churnPos + 1) % kChurnSlots);
    }
    void benchActivity(uint32_t now)  { _benchAt = now ? now : 1; }
    void setEnabled(bool on)          { _enabled = on; }
    bool enabled() const              { return _enabled; }

    // healBootsInRow: self-heal reboots since the last power-on / healthy run
    // (kept by the node in RTC memory). bootMs: when setup() finished.
    // moduleLastRxMs: last byte from the module (0 = never this boot).
    Decision tick(uint32_t now, uint32_t bootMs, uint32_t moduleLastRxMs, bool safe, uint8_t healBootsInRow) {
        Decision d;
        const Why w = trigger(now, bootMs, moduleLastRxMs);
        if (w == Why::NONE) { _pendingSince = 0; _pending = Why::NONE; return d; }
        if (_pending == Why::NONE) { _pending = w; _pendingSince = now ? now : 1; }
        if (!_enabled) return d;
        if (_benchAt && (int32_t)(now - _benchAt) < (int32_t)_c.benchHoldMs) return d;
        if (healBootsInRow >= _c.maxHealBoots) return d;
        if (safe) { d.why = w; return d; }
        if (w == Why::MODULE_SILENT && (int32_t)(now - _pendingSince) >= (int32_t)_c.deferMaxMs) {
            d.why = w; d.forced = true;
        }
        return d;
    }

    Why      pending() const { return _pending; }        // a trigger is active (rebooting or held)
    uint32_t pendingForMs(uint32_t now) const { return _pending == Why::NONE ? 0 : now - _pendingSince; }
    bool     benchHeld(uint32_t now) const {
        return _benchAt && (int32_t)(now - _benchAt) < (int32_t)_c.benchHoldMs;
    }
    uint8_t  churnCount(uint32_t now) const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < kChurnSlots; i++)
            if (_churn[i] && (int32_t)(now - _churn[i]) < (int32_t)_c.churnWindowMs) n++;
        return n;
    }
    uint8_t  startFails() const { return _startFails; }
    const Config& config() const { return _c; }

private:
    Why trigger(uint32_t now, uint32_t bootMs, uint32_t lastRx) const {
        const uint32_t heard = lastRx ? lastRx : bootMs;
        if ((int32_t)(now - heard) >= (int32_t)_c.moduleSilentMs) return Why::MODULE_SILENT;
        if (_startFails >= _c.startFailsMax) return Why::START_FAILS;
        if (churnCount(now) >= _c.churnMax) return Why::LOOP_CHURN;
        return Why::NONE;
    }

    static constexpr uint8_t kChurnSlots = 8;
    Config   _c;
    uint8_t  _startFails = 0;
    uint32_t _churn[kChurnSlots] = {};
    uint8_t  _churnPos = 0;
    uint32_t _benchAt = 0;
    bool     _enabled = true;
    Why      _pending = Why::NONE;
    uint32_t _pendingSince = 0;
};

}  // namespace selfheal
