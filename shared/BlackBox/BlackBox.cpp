#include "BlackBox.h"
#include <esp_system.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_task_wdt.h>

#ifndef FW_GIT
#define FW_GIT "unknown"
#endif

BlackBox Log;

#define BB_RAM_SIZE   4096
#define BB_FILE_MAX   (128UL * 1024UL)   // rotate /bb0.log at this size
#define BB_FILES      8                  // /bb0.log (newest) .. /bb7.log (oldest)
#define BB_FLUSH_MS   10000UL
#define BB_MAGIC      0xB1ACB0C5u

// Unflushed text. .noinit = not zeroed at boot, so a soft reset (panic, WDT,
// ESP.restart, usually brownout) leaves it readable by the next boot. A power
// cut leaves garbage, which the magic + checksum reject.
struct RamLog {
    uint32_t magic, len, boot, sum;
    char     buf[BB_RAM_SIZE];
};
static __NOINIT_ATTR RamLog s_ram;

static uint32_t ramSum(const RamLog& r) {
    uint32_t s = r.boot * 2654435761u;
    for (uint32_t i = 0; i < r.len; i++) s += (uint8_t)r.buf[i];
    return s;
}

// The current line, kept so a WARN-type line can force a flush once it ends.
static char     s_line[200];
static uint16_t s_lineLen = 0;
static bool     s_writing = false;          // no recursion while syncing/dumping

static const char* fileName(uint8_t i) {
    static char n[12];
    snprintf(n, sizeof(n), "/bb%u.log", (unsigned)i);
    return n;
}

// Which /bbN.log exist, from one directory listing. LittleFS.open()/exists() on
// a missing file prints a framework error to the serial line; this doesn't.
static uint16_t presentFiles() {
    uint16_t mask = 0;
    File root = LittleFS.open("/");
    if (!root) return 0;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        const char* n = f.name();
        if (n[0] == '/') n++;
        unsigned i;
        if (sscanf(n, "bb%u.log", &i) == 1 && i < 16) mask |= (uint16_t)(1u << i);
        f.close();
    }
    root.close();
    return mask;
}

static bool urgentLine(const char* l) {
    static const char* const kMarks[] = { "WARN", "!!", "FATAL", "STALL", "FAULT", "FAIL", "BROWNOUT", "rebooting", "HEAL" };
    for (const char* m : kMarks) if (strstr(l, m)) return true;
    return false;
}

void BlackBox::begin(const char* node, const char* resetReason, bool resetWasPowerOn) {
    const bool ramOk = s_ram.magic == BB_MAGIC && s_ram.len <= BB_RAM_SIZE && s_ram.sum == ramSum(s_ram);

    _mounted = LittleFS.begin(true, "/littlefs", 4, "spiffs");

    Preferences p;
    if (p.begin("blackbox", false)) {
        _boot = p.getUInt("boots", 0) + 1;
        p.putUInt("boots", _boot);
        p.end();
    }

    // The previous boot's last seconds (what it was doing when it died).
    if (_mounted && ramOk && s_ram.len > 0) {
        File f = LittleFS.open(fileName(0), FILE_APPEND);
        if (f) {
            f.printf("\n--- %u bytes of boot #%u's log recovered from RAM (reset before they were saved) ---\n",
                     (unsigned)s_ram.len, (unsigned)s_ram.boot);
            f.write((const uint8_t*)s_ram.buf, s_ram.len);
            f.print("\n--- end of recovered text ---\n");
            f.close();
        } else {
            _writeErrs++;
        }
    }
    s_ram.magic = BB_MAGIC; s_ram.boot = _boot; s_ram.len = 0; s_ram.sum = ramSum(s_ram);
    _lastFlushMs = millis();
    echoFor(180000);                                   // boot lines visible on a bench for 3 min

    printf("\n===== %s BOOT #%u  reset=%s  fw=%s built %s %s =====\n",
           node, (unsigned)_boot, resetReason, FW_GIT, __DATE__, __TIME__);
    if (!_mounted) println("[bb] !! FAIL flash log not mounted — logging to serial only");
    else if (!ramOk && !resetWasPowerOn)
        println("[bb] WARN no RAM tail from the previous boot (reset cleared it, or a power cut)");
    else if (resetWasPowerOn)
        println("[bb] power-on boot: the previous boot's last <= 10 s were not saved (power was cut)");
    sync();
}

size_t BlackBox::write(uint8_t c) { return write(&c, 1); }

void BlackBox::echoFor(uint32_t ms) {
    const uint32_t until = millis() + ms;
    if (!echoing() || (int32_t)(until - _echoUntil) > 0) _echoUntil = until ? until : 1;
}
bool BlackBox::echoing() const { return _echoUntil && (int32_t)(_echoUntil - millis()) > 0; }

size_t BlackBox::write(const uint8_t* buf, size_t n) {
    if (echoing()) Serial.write(buf, n);
    if (s_writing) return n;
    bool urgent = false;
    for (size_t i = 0; i < n; i++) {
        if (s_ram.len >= BB_RAM_SIZE) sync();
        if (s_ram.len >= BB_RAM_SIZE) s_ram.len = 0;          // flash dead: keep the newest text only
        const char c = (char)buf[i];
        s_ram.buf[s_ram.len++] = c;
        s_ram.sum += (uint8_t)c;
        if (c == '\n') {
            s_line[s_lineLen] = 0;
            if (urgentLine(s_line)) urgent = true;
            s_lineLen = 0;
        } else if (s_lineLen < sizeof(s_line) - 1) {
            s_line[s_lineLen++] = c;
        }
    }
    if (urgent) sync();
    return n;
}

void BlackBox::tick() {
    if (s_ram.len && (millis() - _lastFlushMs) >= BB_FLUSH_MS) sync();
}

void BlackBox::sync() {
    _lastFlushMs = millis();
    if (!s_ram.len || s_writing) return;
    if (_mounted) {
        s_writing = true;
        File f = LittleFS.open(fileName(0), FILE_APPEND);
        if (f) {
            const size_t w = f.write((const uint8_t*)s_ram.buf, s_ram.len);
            f.close();
            if (w != s_ram.len) _writeErrs++;
            _flushes++;
            rotateIfNeeded();
        } else {
            _writeErrs++;
        }
        s_writing = false;
    }
    s_ram.len = 0;
    s_ram.sum = ramSum(s_ram);
}

void BlackBox::rotateIfNeeded() {
    File f = LittleFS.open(fileName(0), FILE_READ);
    const size_t sz = f ? f.size() : 0;
    if (f) f.close();
    const bool full = LittleFS.usedBytes() > LittleFS.totalBytes() * 85 / 100;
    if (sz < BB_FILE_MAX && !full) return;
    const uint16_t have = presentFiles();
    if (have & (1u << (BB_FILES - 1))) LittleFS.remove(fileName(BB_FILES - 1));
    for (int i = BB_FILES - 2; i >= 0; i--) {
        char from[12]; strcpy(from, fileName(i));
        if (have & (1u << i)) LittleFS.rename(from, fileName(i + 1));
    }
    _rotations++;
}

static void dumpFile(Print& out, uint8_t i, size_t from) {
    File f = LittleFS.open(fileName(i), FILE_READ);
    if (!f) return;
    out.printf("\n----- %s (%u bytes) -----\n", fileName(i), (unsigned)f.size());
    if (from) f.seek(from);
    uint8_t chunk[256];
    while (f.available()) {
        const int r = f.read(chunk, sizeof(chunk));
        if (r <= 0) break;
        out.write(chunk, r);
        esp_task_wdt_reset();                                 // a full dump takes ~90 s at 115200
    }
    f.close();
}

void BlackBox::dumpAll(Print& out) { dumpTail(out, UINT32_MAX); }

void BlackBox::dumpTail(Print& out, uint32_t bytes) {
    sync();
    out.printf("\n===== BLACKBOX DUMP BEGIN boot=#%u fw=%s up=%lus =====\n",
               (unsigned)_boot, FW_GIT, (unsigned long)(millis() / 1000));
    if (_mounted) {
        s_writing = true;
        // Walk newest -> oldest to find where `bytes` starts, then print oldest first.
        int first = 0; size_t skip = 0; uint32_t total = 0;
        const uint16_t have = presentFiles();
        for (int i = 0; i < BB_FILES; i++) {
            if (!(have & (1u << i))) break;
            File f = LittleFS.open(fileName(i), FILE_READ);
            if (!f) break;
            const size_t sz = f.size(); f.close();
            first = i;
            if (total + sz >= bytes) { skip = sz - (bytes - total); break; }
            total += sz;
        }
        for (int i = first; i >= 0; i--) dumpFile(out, (uint8_t)i, i == first ? skip : 0);
        s_writing = false;
    }
    out.println("\n===== BLACKBOX DUMP END =====");
}

void BlackBox::stat(Print& out) {
    out.printf("[bb] boot=#%u fw=%s mounted=%s used=%u/%u bytes flushes=%lu writeErrs=%lu rotations=%lu\n",
               (unsigned)_boot, FW_GIT, _mounted ? "yes" : "NO",
               _mounted ? (unsigned)LittleFS.usedBytes() : 0, _mounted ? (unsigned)LittleFS.totalBytes() : 0,
               (unsigned long)_flushes, (unsigned long)_writeErrs, (unsigned long)_rotations);
    if (!_mounted) return;
    const uint16_t have = presentFiles();
    for (int i = 0; i < BB_FILES; i++) {
        if (!(have & (1u << i))) continue;
        File f = LittleFS.open(fileName(i), FILE_READ);
        if (!f) continue;
        out.printf("[bb]   %s %u bytes\n", fileName(i), (unsigned)f.size());
        f.close();
    }
}

void BlackBox::clear() {
    s_ram.len = 0; s_ram.sum = ramSum(s_ram);
    if (_mounted) { const uint16_t have = presentFiles();
        for (int i = 0; i < BB_FILES; i++) if (have & (1u << i)) LittleFS.remove(fileName(i)); }
    println("[bb] log cleared");
    sync();
}

bool BlackBox::command(const String& line) {
    if (line == "log") { dumpAll(Serial); return true; }
    if (line == "log tail" || line.startsWith("log tail ")) {
        const int kb = line.length() > 9 ? line.substring(9).toInt() : 16;
        dumpTail(Serial, (uint32_t)(kb > 0 ? kb : 16) * 1024UL);
        return true;
    }
    if (line == "log stat") { stat(Serial); return true; }
    if (line.startsWith("log mark ")) {
        logTs(); printf("[bb] MARK %s\n", line.substring(9).c_str());
        sync();
        return true;
    }
    if (line == "log clear yes") { clear(); return true; }
    if (line == "bbtest reboot" || line == "bbtest panic" || line == "bbtest hang") {
        // Bench proof that a reset keeps the unsaved RAM tail: the next boot must
        // show "recovered from RAM" with this line in it. Deliberately no sync().
        logTs(); printf("[bb] bbtest: unsaved line before %s\n", line.c_str() + 7);
        delay(50);
        if (line.endsWith("reboot")) ESP.restart();
        else if (line.endsWith("panic")) abort();
        else while (true) { }                          // the loop watchdog (60 s) must reset it
    }
    return false;
}

void logTs() {
    const uint32_t ms = millis();
    Log.printf("[%02u:%02u.%03u] ", ms / 60000, (ms % 60000) / 1000, ms % 1000);
}

const char* resetReasonName() {
    static const char* const kReason[] = { "UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT",
                                           "TASK_WDT", "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO" };
    const unsigned r = (unsigned)esp_reset_reason();
    return r < sizeof(kReason) / sizeof(kReason[0]) ? kReason[r] : "?";
}

// ── Self-heal reboot counter (SelfHeal.h guard) ──────────────────────────────
// RTC slow memory, not initialised at boot: survives ESP.restart / WDT / panic.
// A power-on leaves garbage there, which the magic rejects (count = 0).
struct HealRtc { uint32_t magic; uint8_t count; uint8_t justHealed; };
static RTC_NOINIT_ATTR HealRtc s_heal;
static bool s_healThisBoot = false;
#define HEAL_MAGIC 0x4EA15E1Fu

uint8_t healBootsBegin() {
    if (s_heal.magic != HEAL_MAGIC || esp_reset_reason() == ESP_RST_POWERON) {
        s_heal.magic = HEAL_MAGIC; s_heal.count = 0; s_heal.justHealed = 0;
    }
    s_healThisBoot = s_heal.justHealed && esp_reset_reason() == ESP_RST_SW;
    s_heal.justHealed = 0;
    return s_heal.count;
}
bool    healBootThisBoot() { return s_healThisBoot; }
void    healBootsMark()    { s_heal.magic = HEAL_MAGIC; if (s_heal.count < 255) s_heal.count++; s_heal.justHealed = 1; }
void    healBootsClear()   { s_heal.count = 0; }
uint8_t healBoots()        { return s_heal.magic == HEAL_MAGIC ? s_heal.count : 0; }
