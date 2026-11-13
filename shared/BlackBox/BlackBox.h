#pragma once

#include <Arduino.h>

// =============================================================================
// Black-box log (2026-10-01, Spooky Tree; shared since the same day for every
// YX5300 prop: Tree, Pumpkin Men, Patch L/R). The Tree went silent on 09-29
// and 09-30 with no record of why: it logged to USB serial only, and opening
// the DevKit's port reboots it (docs/hardware-gotchas.md §6), so the stuck
// state was gone before anyone could look. Everything the firmware prints now
// also goes here.
//
//   RAM   4 KB buffer in .noinit DRAM: survives a panic, a watchdog reset,
//         ESP.restart() and (usually) a brownout; recovered into flash at the
//         next boot. Lost only on a real power cut (then <= 10 s of text).
//   Flash LittleFS on the default "spiffs" partition (1.375 MB, on the chip;
//         no SD module). /bb0.log is current; at 128 KB it rotates down to
//         /bb7.log (8 x 128 KB ~ 1 MB, ~2-3 days at the normal log rate).
//         Written every 10 s, when the buffer fills, and at once after any
//         WARN/!!/FATAL/STALL/FAULT/FAIL/HEAL line.
//   Read  serial commands, handled by command(): `log` dumps every file, oldest
//         first, between BEGIN/END markers; `log tail [KB]`, `log stat`,
//         `log mark <text>`, `log clear yes`, `bbtest reboot|panic|hang`.
//         tools/blackbox_pull.sh does it from the Workshop via yard-bridge-pi.
//
// The logger must never take the show down: every flash error is counted and
// logging carries on to serial only. Call it from the loop task only (the
// ESP-NOW callbacks just count; they never print).
//
// Also here, because they need .noinit/RTC memory next to the log:
//   heal-boot counter (SelfHeal.h reboot-loop guard) and the reset-reason name.
// =============================================================================

class BlackBox : public Print {
public:
    // Mount (format on first use), bump the persistent boot counter, recover the
    // RAM tail left by the previous boot, write the boot header.
    void begin(const char* node, const char* resetReason, bool resetWasPowerOn);

    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t n) override;

    void     tick();                 // call every loop(): timed flush
    // Live USB-serial echo (2026-10-01, Joe: "we don't need the serial as much"):
    // the flash log always gets everything; the serial line only while someone
    // may be watching. begin() opens 3 min; every bench command should call
    // echoFor(30 min). Dumps (`log`) always go to serial.
    void     echoFor(uint32_t ms);
    bool     echoing() const;
    void     sync();                 // write the RAM buffer to flash now
    uint32_t bootNo() const { return _boot; }

    // Bench/pull commands (see above). true = the line was one of ours.
    bool command(const String& line);

    void dumpAll(Print& out);                   // every file, oldest first
    void dumpTail(Print& out, uint32_t bytes);  // last `bytes` across the files
    void stat(Print& out);
    void clear();                               // delete every log file

private:
    void     rotateIfNeeded();
    uint32_t _boot = 0;
    bool     _mounted = false;
    uint32_t _lastFlushMs = 0;
    uint32_t _flushes = 0, _writeErrs = 0, _rotations = 0;
    uint32_t _echoUntil = 0;
};

extern BlackBox Log;

// "[mm:ss.mmm] " prefix into the log (every node's printTs()).
void logTs();

// esp_reset_reason() as text ("POWERON", "TASK_WDT", ...).
const char* resetReasonName();

// Self-heal reboots in a row (RTC memory: survives ESP.restart, cleared by a
// power-on). healBootsBegin() once at boot; healBootsMark() just before a
// self-heal ESP.restart(); healBootsClear() after a healthy run.
uint8_t healBootsBegin();   // returns the count; also says if this boot was a self-heal
bool    healBootThisBoot();
void    healBootsMark();
void    healBootsClear();
uint8_t healBoots();
