#include "skeletons.h"
#include "brightsign.h"
#include <Preferences.h>

// ── tunables (saved in NVS "skel", change from the console, no reflash) ─────────
struct SkelCfg {
    int32_t lagMs   = 250;   // card sends NOW a little before the first frame/sound: shift everything later
    int32_t fallMs  = 80;    // power off -> audible clunk (jaw drop time)
    int32_t openMs  = 250;   // power on this long before the jaw is fully open
    int32_t dj      = 25;    // % of clips WITH cues that get a moment, DJ loop
    int32_t show    = 50;    // ... during the trick-or-treat show
    int32_t gapS    = 120;   // at least this long between moments (DJ); show uses gapS/4
    bool    enabled = true;
    bool    activeHigh = true;   // Joe: "high" trigger module
};
static SkelCfg cfg;
static Preferences prefs;

static const uint32_t MAX_HOLD_MS = 5000;     // one hold never longer than this
static const uint32_t MAX_ON_MS   = 7000;     // hard cutoff, whatever asked for it
static const uint32_t MIN_OFF_MS  = 80;       // between a clunk and the next open
static const uint32_t CUES_WINDOW_MS = 2000;  // CUES must follow its NOW this closely

struct Ev { uint32_t at; uint8_t mask; bool on; };
static Ev       s_ev[48];
static int      s_evN = 0;
static uint8_t  s_on = 0;                     // bit0 = L, bit1 = R
static uint32_t s_onSince[2] = {0, 0};
static int      s_nowN = -1;
static uint32_t s_nowMs = 0;
static uint32_t s_lastMomentEnd = 0;
static bool     s_haveMoment = false;
static bool     s_narrator = false;
static bool     s_forceNext = false, s_always = false;
static String   s_last = "none";
static uint32_t s_ver = 1;

static void ts() { Serial.printf("[%lu] ", (unsigned long)millis()); }

static void setLast(const String& s) {
    s_last = s; s_ver++;
    ts(); Serial.printf("[skel] %s\n", s.c_str());
}

static void drive(uint8_t mask) {
    for (int i = 0; i < 2; i++) {
        bool on = mask & (1 << i);
        bool was = s_on & (1 << i);
        if (on && !was) s_onSince[i] = millis();
        digitalWrite(i ? SKEL_PIN_R : SKEL_PIN_L, (on == cfg.activeHigh) ? HIGH : LOW);
    }
    s_on = mask;
}

static void load() {
    prefs.begin("skel", true);
    cfg.lagMs = prefs.getInt("lag", cfg.lagMs);
    cfg.fallMs = prefs.getInt("fall", cfg.fallMs);
    cfg.openMs = prefs.getInt("open", cfg.openMs);
    cfg.dj = prefs.getInt("dj", cfg.dj);
    cfg.show = prefs.getInt("show", cfg.show);
    cfg.gapS = prefs.getInt("gap", cfg.gapS);
    cfg.enabled = prefs.getBool("en", cfg.enabled);
    cfg.activeHigh = prefs.getBool("hi", cfg.activeHigh);
    prefs.end();
}

static void save() {
    prefs.begin("skel", false);
    prefs.putInt("lag", cfg.lagMs); prefs.putInt("fall", cfg.fallMs); prefs.putInt("open", cfg.openMs);
    prefs.putInt("dj", cfg.dj); prefs.putInt("show", cfg.show); prefs.putInt("gap", cfg.gapS);
    prefs.putBool("en", cfg.enabled); prefs.putBool("hi", cfg.activeHigh);
    prefs.end();
}

void skel_begin() {
    // Relays off before anything else: until here the pins float (module input pull-down).
    pinMode(SKEL_PIN_L, OUTPUT);
    pinMode(SKEL_PIN_R, OUTPUT);
    digitalWrite(SKEL_PIN_L, LOW);
    digitalWrite(SKEL_PIN_R, LOW);
    load();
    drive(0);
}

static void clearEvents() { s_evN = 0; }

static void addEv(uint32_t at, uint8_t mask, bool on) {
    if (s_evN >= (int)(sizeof(s_ev) / sizeof(s_ev[0]))) return;
    // keep sorted by time (small list, insertion)
    int i = s_evN++;
    while (i > 0 && (int32_t)(s_ev[i - 1].at - at) > 0) { s_ev[i] = s_ev[i - 1]; i--; }
    s_ev[i] = {at, mask, on};
}

void skel_stop(const char* why) {
    bool busy = s_on || s_evN;
    // a moment cut short ends now (the gap counts from here, not from its planned end)
    if (s_haveMoment && (int32_t)(s_lastMomentEnd - millis()) > 0) s_lastMomentEnd = millis();
    clearEvents();
    drive(0);
    if (busy) { ts(); Serial.printf("[skel] stop (%s)\n", why); }
}

static uint8_t maskOf(char c) { return c == 'L' ? 1 : c == 'R' ? 2 : c == 'B' ? 3 : 0; }

// Schedule one group relative to `anchor` (ms when the clip started). Returns the moment's end
// (ms) or 0 if nothing could be scheduled.
static uint32_t schedule(const char* g, uint32_t anchor) {
    if (!g || strlen(g) < 3) return 0;
    char type = g[0];
    uint8_t mask = maskOf(g[1]);
    if (!mask) return 0;
    const char* p = g + 2;
    uint32_t now = millis();
    uint32_t base = anchor + cfg.lagMs;
    if (type == 'H') {
        float a = atof(p);
        const char* dash = strchr(p, '-');
        if (!dash) return 0;
        float b = atof(dash + 1);
        uint32_t on = base + (uint32_t)(a * 1000) - cfg.openMs;     // fully open as the peak hits
        uint32_t off = base + (uint32_t)(b * 1000) - cfg.fallMs;    // clunk as it ends
        if (off - on > MAX_HOLD_MS) off = on + MAX_HOLD_MS;
        if ((int32_t)(on - now) < 0) return 0;                     // already past
        addEv(on, mask, true);
        addEv(off, mask, false);
        return off;
    }
    if (type == 'K') {
        uint32_t prevOff = 0, last = 0;
        int n = 0, skipped = 0;
        while (*p) {
            float t = atof(p);
            uint32_t off = base + (uint32_t)(t * 1000) - cfg.fallMs;  // the clunk lands on the hit
            uint32_t on = off - cfg.openMs;
            if (prevOff && (int32_t)(on - (prevOff + MIN_OFF_MS)) < 0) on = prevOff + MIN_OFF_MS;
            // jaw needs at least ~60% of its open time to make a real clunk
            if ((int32_t)(on - now) > 0 && (int32_t)(off - on) >= cfg.openMs * 6 / 10) {
                addEv(on, mask, true);
                addEv(off, mask, false);
                prevOff = last = off;
                n++;
            } else {
                skipped++;
            }
            const char* c = strchr(p, ',');
            if (!c) break;
            p = c + 1;
        }
        if (skipped) { ts(); Serial.printf("[skel] clack: %d hits too close/late, skipped\n", skipped); }
        return n ? last : 0;
    }
    return 0;
}

void skel_on_now(int n) {
    skel_stop("new clip");
    s_nowN = n;
    s_nowMs = millis();
}

void skel_on_cues(int n, const char* groups) {
    char why[96];
    if (n != s_nowN || millis() - s_nowMs > CUES_WINDOW_MS) {
        snprintf(why, sizeof why, "CUES %d ignored: not for the clip that just started (%d)", n, s_nowN);
        setLast(why); return;
    }
    if (!cfg.enabled) return;   // quiet: switched off on purpose
    if (s_narrator) { snprintf(why, sizeof why, "#%d skipped: narrator has the floor", n); setLast(why); return; }

    bool show = strcmp(brightsign_card_mode(), "SHOW") == 0;
    uint32_t gapMs = (uint32_t)cfg.gapS * 1000 / (show ? 4 : 1);
    bool forced = s_forceNext || s_always;
    if (!forced) {
        // signed: a moment cut short by SKIP/new clip has its planned end still in the future
        // (unsigned wrapped and skipped the gap, bench 2026-10-02)
        if (s_haveMoment && (int32_t)(millis() - s_lastMomentEnd) < (int32_t)gapMs) {
            ts(); Serial.printf("[skel] #%d: last moment %ld s ago, need %lu s\n", n,
                                (long)((int32_t)(millis() - s_lastMomentEnd) / 1000), (unsigned long)(gapMs / 1000));
            return;
        }
        long roll = random(100);
        long chance = show ? cfg.show : cfg.dj;
        if (roll >= chance) {
            ts(); Serial.printf("[skel] #%d: dice %ld >= %ld%%, no moment this time\n", n, roll, chance);
            return;
        }
    }
    // split groups (space separated); best first gets 60 %, otherwise any other
    char buf[200];
    strlcpy(buf, groups, sizeof buf);
    const char* g[8]; int ng = 0;
    for (char* t = strtok(buf, " "); t && ng < 8; t = strtok(nullptr, " ")) g[ng++] = t;
    if (!ng) return;
    // Bench (NEXT / ALWAYS) = always the best moment, so a test is repeatable.
    int pick = (ng == 1 || forced || random(100) < 60) ? 0 : 1 + random(ng - 1);
    uint32_t end = schedule(g[pick], s_nowMs);
    if (!end && pick) { pick = 0; end = schedule(g[0], s_nowMs); }
    if (!end) { snprintf(why, sizeof why, "#%d: moment '%.40s' could not be scheduled", n, g[pick]); setLast(why); return; }
    s_forceNext = false;
    s_haveMoment = true;
    s_lastMomentEnd = end;
    snprintf(why, sizeof why, "#%d %s: %.60s", n, show ? "show" : "dj", g[pick]);
    setLast(why);
}

void skel_set_narrator(bool speaking) {
    s_narrator = speaking;
    if (speaking) skel_stop("narrator");
}

void skel_tick() {
    uint32_t now = millis();
    while (s_evN && (int32_t)(now - s_ev[0].at) >= 0) {
        Ev e = s_ev[0];
        for (int i = 1; i < s_evN; i++) s_ev[i - 1] = s_ev[i];
        s_evN--;
        drive(e.on ? (s_on | e.mask) : (s_on & ~e.mask));
    }
    for (int i = 0; i < 2; i++) {
        if ((s_on & (1 << i)) && now - s_onSince[i] > MAX_ON_MS) {
            drive(s_on & ~(1 << i));
            setLast(String("!! SAFETY: ") + (i ? "right" : "left") + " jaw on > 7 s, forced off");
        }
    }
    // Lost the card = we don't know what's playing.
    static bool linkWas = true;
    bool link = brightsign_link_ok();
    if (linkWas && !link) skel_stop("card link lost");
    linkWas = link;
}

bool skel_enabled() { return cfg.enabled; }
void skel_set_enabled(bool on) {
    if (cfg.enabled == on) return;
    cfg.enabled = on;
    save();
    if (!on) skel_stop("disabled");
    setLast(on ? "enabled" : "disabled");
}
const String& skel_last() { return s_last; }
uint32_t skel_version() { return s_ver; }

static void status() {
    ts(); Serial.printf("[skel] relays L=GPIO%d R=GPIO%d, %s, trigger %s, jaws L=%s R=%s, %d events queued, narrator=%d, "
                        "lag %ld fall %ld open %ld ms, chance DJ %ld%% show %ld%%, gap %ld s%s%s, last: %s\n",
                        SKEL_PIN_L, SKEL_PIN_R, cfg.enabled ? "ENABLED" : "DISABLED", cfg.activeHigh ? "HIGH" : "LOW",
                        (s_on & 1) ? "OPEN" : "closed", (s_on & 2) ? "OPEN" : "closed", s_evN, (int)s_narrator,
                        (long)cfg.lagMs, (long)cfg.fallMs, (long)cfg.openMs, (long)cfg.dj, (long)cfg.show, (long)cfg.gapS,
                        s_forceNext ? ", NEXT forced" : "", s_always ? ", ALWAYS (bench)" : "", s_last.c_str());
}

bool skel_console(const char* line) {
    if (strncasecmp(line, "SKEL", 4) != 0 || (line[4] && line[4] != ' ')) return false;
    char w[16] = "", a[16] = "";
    sscanf(line + 4, "%15s %15s", w, a);
    for (char* c = w; *c; c++) *c = toupper(*c);
    for (char* c = a; *c; c++) *c = toupper(*c);
    int v = atoi(a);
    uint32_t now = millis();
    if (!w[0]) { status(); return true; }
    if (!strcmp(w, "ON") || !strcmp(w, "OFF")) { skel_set_enabled(!strcmp(w, "ON")); status(); return true; }
    if (!strcmp(w, "STOP")) { skel_stop("console"); return true; }
    if (!strcmp(w, "NEXT")) { s_forceNext = true; ts(); Serial.println(F("[skel] next clip with cues gets a moment")); return true; }
    if (!strcmp(w, "ALWAYS")) { s_always = !strcmp(a, "ON"); status(); return true; }
    if (maskOf(w[0]) && !w[1]) {                       // L / R / B [ms]  (manual, ignores enable)
        uint32_t ms = v > 0 ? min(v, (int)(MAX_ON_MS - 500)) : 800;
        skel_stop("manual");
        addEv(now, maskOf(w[0]), true);
        addEv(now + ms, maskOf(w[0]), false);
        ts(); Serial.printf("[skel] manual %s jaw %lu ms\n", w, (unsigned long)ms);
        return true;
    }
    if (!strcmp(w, "CLACK")) {                         // n clunks 0.6 s apart, both jaws
        int n = v > 0 ? min(v, 10) : 4;
        skel_stop("manual");
        for (int i = 0; i < n; i++) {
            uint32_t off = now + cfg.openMs + 100 + i * 600;
            addEv(off - cfg.openMs, 3, true);
            addEv(off, 3, false);
        }
        ts(); Serial.printf("[skel] clack test x%d (open %ld ms)\n", n, (long)cfg.openMs);
        return true;
    }
    if (!strcmp(w, "POL")) {
        cfg.activeHigh = strcmp(a, "LOW") != 0;
        save(); drive(0); status(); return true;
    }
    int32_t* f = !strcmp(w, "LAG") ? &cfg.lagMs : !strcmp(w, "FALL") ? &cfg.fallMs : !strcmp(w, "OPEN") ? &cfg.openMs
               : !strcmp(w, "DJ") ? &cfg.dj : !strcmp(w, "SHOW") ? &cfg.show : !strcmp(w, "GAP") ? &cfg.gapS : nullptr;
    if (f && a[0]) {
        int lo = 0, hi = (f == &cfg.dj || f == &cfg.show) ? 100 : (f == &cfg.gapS ? 3600 : 2000);
        *f = constrain(v, lo, hi);
        save(); status(); return true;
    }
    ts(); Serial.println(F("[skel] ?? SKEL [ON|OFF|STOP|NEXT|ALWAYS ON|OFF|L|R|B [ms]|CLACK [n]|POL HIGH|LOW|LAG|FALL|OPEN|DJ|SHOW|GAP n]"));
    return true;
}
