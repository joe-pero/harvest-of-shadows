#include "hud.h"
#include "lgfx_conf.h"
#include "dmx_state.h"
#include <math.h>

// =============================================================================
// DARROW CHEMICAL CO. // 2-4-5 TRIOXIN containment monitor  —  LANDSCAPE 480x320
// 7 phase-reactive states (dmx_show_state / HS_*):
//   CONTAINED  swamp/safe  |  WARNING  unstable  |  GLASS BREAK  gas release
//   BROKEN  specimen loose / meltdown  |  DOUSE  accelerant dumped on specimen
//   (2026-09-21) |  INCINERATE  fire purging the specimen (2026-09-21, RED)
//   |  RESET  recontainment -> loop (bench-only in the live show now — see
//   dmx_show_state(), the real timeline maps the tail straight to CONTAINED)
// Clean instrument look. No scanlines / no glitch (readability first).
//
// Burn-in protection (2026-09-07, Joe): the panel can sit on CONTAINED for hours
// between shows, so —
//   1. PIXEL ORBIT: the whole composition shifts +/-6 px on a slow 8-point ring,
//      stepping every ORBIT_MS. Always on, every state.
//   2. DIM: CONTAINED / RESET run the backlight lower than the alarm states.
// A separate minimal "STANDBY" card (dim, wandering, mostly blank) used to
// auto-replace the HUD after IDLE_AFTER_MS of CONTAINED — REMOVED 2026-09-19
// (Joe: "not what we built... needs the other one — that fake HUD display
// with random readings but contained"). The real, fully-animated CONTAINED
// HUD (PPM/EKG/telemetry log all still moving, per logTick()/ekgAdvance()) IS
// now the permanent passive/idle display — the pixel orbit above already
// covers burn-in, so no separate low-content screen is needed. drawIdle() and
// MODE_IDLE are kept only as a bench preview (`idle` serial cmd /
// hud_force_idle()) — they never trigger automatically anymore.
// =============================================================================

static LGFX        lcd;
static LGFX_Sprite ekg(&lcd);
static bool  s_enabled = true, s_inited = false;
static int   s_lastState = -99;
static uint32_t s_bootMs = 0;

// ── burn-in mitigation state ───────────────────────────────────────────────
static const uint32_t ORBIT_MS      = 90000;     // pixel-shift step interval
static bool s_forceIdle = false;                 // bench: jump straight to STANDBY (preview only, see file header)
static int      g_ox = 0, g_oy = 0;              // current orbit offset
static uint32_t s_orbAt = 0;
static uint8_t  s_orbIx = 0;
enum { MODE_HUD = 0, MODE_IDLE = 1, MODE_OVERRIDE = 2, MODE_FREEZE = 6 };
static uint8_t  s_mode = MODE_HUD;
static uint32_t s_idleMoveAt = 0;
static int      s_idleX = 40, s_idleY = 40;

// ── touch override state (2026-09-21 redesign — single full-screen button) ──
static bool     s_touchDown   = false;           // edge-detect for tap
static bool     s_dirty       = true;            // current overlay screen needs a (re)draw
static uint32_t s_menuTouchAt = 0;
static const uint32_t MENU_TIMEOUT_MS = 30000;   // override screen -> back to monitor if untouched

static const int W = 480, H = 320;
static const int HDR_H = 42, PAD = 8, COL_W = 226;

// design (un-shifted) anchors — heights/widths are constant, origins get the orbit
static const int D_ST_Y = HDR_H + 4,        ST_H = 80;
static const int D_PP_Y = D_ST_Y + ST_H + 4, PP_H = 82;
static const int D_IN_Y = D_PP_Y + PP_H + 4, IN_H = 78;
static const int D_EK_Y = HDR_H + 4,        EK_H = 104;
static const int D_LG_Y = D_EK_Y + EK_H + 4;
static const int LG_H  = 298 - D_LG_Y;
static const int EKG_W = COL_W - 16, EKG_H = EK_H - 48;

#define COL_L  (PAD + g_ox)
#define COL_R  (PAD + COL_W + PAD + g_ox)
#define ST_Y   (D_ST_Y + g_oy)
#define PP_Y   (D_PP_Y + g_oy)
#define IN_Y   (D_IN_Y + g_oy)
#define EK_Y   (D_EK_Y + g_oy)
#define LG_Y   (D_LG_Y + g_oy)
#define EKG_X  (PAD + COL_W + PAD + 8 + g_ox)
#define EKG_Y  (D_EK_Y + 22 + g_oy)

#define LOGN 8
static String s_log[LOGN];

// ── palette per state ──────────────────────────────────────────────────────
struct Pal { uint16_t bg, panel, edge, fg, dim, hot, hdrtx; };
static Pal pal;
static void setPalette(int st) {
    switch (st) {
        case HS_CONTAINED: pal = { 0x0080, 0x0180, 0x0560, 0x2FEC, 0x0540, 0xAFF5, 0xCFF9 }; break;
        case HS_WARNING:   pal = { 0x2000, 0x3000, 0xA320, 0xFD40, 0x8200, 0xFFE0, 0xFFE0 }; break;
        case HS_GLASSBREAK:pal = { 0x4000, 0x6000, 0xF800, 0xFFFF, 0xFC00, 0xFFFF, 0xFFFF }; break;
        case HS_BROKEN:    pal = { 0x2104, 0x3186, 0x8410, 0xF7BE, 0xA534, 0xF9E7, 0xFFFF }; break;
        // DOUSE — incineration fluid dumped on Specimen 04 (2026-09-21). Deep
        // amber/brown, distinct from WARNING's pure red and BROKEN's grey-red.
        case HS_DOUSE:      pal = { 0x30E0, 0x4140, 0xFC40, 0xFED1, 0x8AC2, 0xFC40, 0xFED1 }; break;
        // INCINERATE — fire actively purging the specimen (2026-09-21). Hot
        // fire red-orange, more saturated/intense than WARNING's alert red.
        case HS_INCINERATE: pal = { 0x3840, 0x58A0, 0xF9E0, 0xFE0F, 0x9981, 0xF9E0, 0xFE0F }; break;
        default:           pal = { 0x00C0, 0x0180, 0x0560, 0x2FEC, 0x0540, 0x8FE8, 0xCFF9 }; break; // RESET
    }
}

static void applyBrightness(int st) {
    if (!s_enabled) { lcd.setBrightness(0); return; }
    if (s_mode == MODE_IDLE)                        lcd.setBrightness(70);
    else if (st == HS_CONTAINED || st == HS_RESET)  lcd.setBrightness(165);
    else                                            lcd.setBrightness(210);
}

static void panel(int x, int y, int w, int h, const char* label) {
    lcd.fillRect(x, y, w, h, pal.bg);
    lcd.fillRect(x, y, w, 14, pal.panel);
    lcd.drawRect(x, y, w, h, pal.edge);
    lcd.setFont(&fonts::Font0);
    lcd.setTextColor(pal.fg, pal.panel);
    lcd.setCursor(x + 6, y + 3);
    lcd.print(label);
}
static void body(int x, int y, int w, int h) { lcd.fillRect(x + 1, y + 15, w - 2, h - 16, pal.bg); }

static void trefoil(int cx, int cy, int r, uint16_t c) {
    for (int i = 0; i < 3; i++) {
        float a = -PI / 2 + i * 2 * PI / 3;
        int px = cx + (int)(r * 0.60f * cosf(a)), py = cy + (int)(r * 0.60f * sinf(a));
        lcd.fillCircle(px, py, (int)(r * 0.50f), c);
        lcd.fillCircle(px, py, (int)(r * 0.22f), pal.panel);
    }
    lcd.fillCircle(cx, cy, (int)(r * 0.22f), c);
    lcd.drawCircle(cx, cy, r, c);
}

// ── telemetry ──────────────────────────────────────────────────────────────
static const char* L_CONT[] = {
    "MONITORING - ATMOSPHERE NOMINAL",
    "VAPOR SEAL .......... INTACT",
    "SPECIMEN 04 ......... DORMANT",
    "CRYO LOOP -18.0 C ... STABLE",
    "PERIMETER SITE-D .... SECURE",
    "CUSTODY DARROW CHEM / U.S. ARMY",
};
static const char* L_WARN[] = {
    "IMPACT DETECTED - CONTAINER WALL",
    "HYDRAULIC LINE RUPTURE  SECTOR D",
    "BUS FAULT - SECTOR D LIGHTING",
    "SPECIMEN 04 STRIKING VIEWPORT",
    "SEAL PRESSURE -0.6 kPa  FALLING",
    "ADVISORY: SECURE CONTAINER",
    "ADVISORY: DO NOT TAMPER",
};
static const char* L_GLASS[] = {
    "VIEWPORT FAILURE",
    "2-4-5 TRIOXIN RELEASE  SECTOR D",
    "BIOHAZARD - EVACUATE",
    "CONTAINMENT BREACH",
};
static const char* L_BROKE[] = {
    "SPECIMEN LOOSE",
    "SENSOR ARRAY OFFLINE",
    "POWER BUS ARC FAULT - RETRYING",
    "CAMERA 4 / 7 / 11  NO SIGNAL",
    "MULTIPLE CONTACTS  D-E-F",
    "SEND MORE PARAMEDICS",
    "SUBJECT VOCALIZING - DISREGARD",
    "RECOMMEND INCINERATE SECTOR D",
};
static const char* L_RESET[] = {
    "VENT PURGE CYCLE ... RUNNING",
    "AMMONIA SCRUBBER ... ONLINE",
    "SPECIMEN 04 ........ RE-SECURED",
    "SEAL RE-PRESSURIZED  +2.1 kPa",
    "ATMOSPHERE CLEARING  ppm FALLING",
    "STANDBY FOR NEXT CYCLE",
};
// DOUSE — fluid deployed, not yet burning (2026-09-21).
static const char* L_DOUSE[] = {
    "ACCELERANT RELEASE - SECTOR D",
    "SPECIMEN 04 SATURATION 40%",
    "CHEMICAL DELUGE SYSTEM ACTIVE",
    "DARROW PROTOCOL 7 INITIATED",
    "CONTAINMENT TEAM CLEARING SECTOR D",
    "STAND BY FOR IGNITION",
};
// INCINERATE — fire purging the specimen (2026-09-21).
static const char* L_INCIN[] = {
    "IGNITION CONFIRMED - SECTOR D",
    "SPECIMEN 04 COMBUSTION IN PROGRESS",
    "THERMAL SENSORS PEGGED",
    "AMBIENT TEMP 900+ C",
    "2-4-5 TRIOXIN - CONSUMED BY FLAME",
    "PURGE CYCLE NOMINAL",
    "AWAITING ASH ANALYSIS",
    "RECOMMEND FULL SECTOR VENT",
};
static uint32_t s_logNext = 0;
static void logPush(const char* s) {
    for (int i = 0; i < LOGN - 1; i++) s_log[i] = s_log[i + 1];
    s_log[LOGN - 1] = s;
}
static void logTick(int st, uint32_t now) {
    uint32_t period = st == HS_CONTAINED ? 4200 : st == HS_RESET ? 2600
                     : st == HS_WARNING ? 1500 : st == HS_GLASSBREAK ? 350
                     : st == HS_DOUSE ? 700 : st == HS_INCINERATE ? 500 : 800;
    if (now < s_logNext) return;
    s_logNext = now + period - (esp_random() % (period / 2));
    const char** p; int n;
    switch (st) {
        case HS_CONTAINED:  p = L_CONT;  n = 6; break;
        case HS_WARNING:    p = L_WARN;  n = 7; break;
        case HS_GLASSBREAK: p = L_GLASS; n = 4; break;
        case HS_RESET:      p = L_RESET; n = 6; break;
        case HS_DOUSE:      p = L_DOUSE; n = 6; break;
        case HS_INCINERATE: p = L_INCIN; n = 8; break;
        default:            p = L_BROKE; n = 8; break;
    }
    logPush(p[esp_random() % n]);
}
static void drawLog(int st) {
    body(COL_R, LG_Y, COL_W, LG_H);
    int lh = 15, y0 = LG_Y + 20;
    lcd.setFont(&fonts::Font0);
    for (int i = 0; i < LOGN; i++) {
        if (!s_log[i].length()) continue;
        bool fresh = (i >= LOGN - 2);
        lcd.setTextColor(fresh ? pal.fg : pal.dim, pal.bg);
        lcd.setCursor(COL_R + 8, y0 + i * lh);
        lcd.print(fresh ? "> " : "  ");
        lcd.print(s_log[i]);
    }
}

// ── EKG ────────────────────────────────────────────────────────────────────
static float s_ekgPh = 0;
static void ekgAdvance(int st) {
    int step = st == HS_CONTAINED || st == HS_RESET ? 2 : st == HS_WARNING ? 3 : 5;
    ekg.scroll(-step, 0);
    for (int c = EKG_W - step; c < EKG_W; c++) {
        ekg.drawFastVLine(c, 0, EKG_H, pal.bg);
        float v;
        if (st == HS_CONTAINED || st == HS_RESET) {
            v = sinf(s_ekgPh * 0.6f) * 0.05f;
            if ((esp_random() & 90) == 0) v += 0.5f;
        } else if (st == HS_WARNING) {
            float t = fmodf(s_ekgPh, 6.2831853f);
            v = (t < 0.20f) ? 0.9f : (t < 0.4f ? -0.4f : sinf(t) * 0.1f);
        } else {
            v = ((int)(esp_random() % 200) - 100) / 100.0f;
        }
        int y = EKG_H / 2 - (int)(v * (EKG_H / 2 - 3));
        ekg.drawPixel(c, y, pal.fg); ekg.drawPixel(c, y + 1, pal.fg);
        s_ekgPh += (st == HS_BROKEN || st == HS_GLASSBREAK || st == HS_DOUSE || st == HS_INCINERATE) ? 0.9f : 0.19f;
    }
    ekg.pushSprite(EKG_X, EKG_Y);
}

// ── chrome ─────────────────────────────────────────────────────────────────
static const char* pillText(int st) {
    return st == HS_CONTAINED ? "CONTAINED" : st == HS_WARNING ? "WARNING"
         : st == HS_GLASSBREAK ? "BREACH"   : st == HS_BROKEN  ? "LOCKDOWN"
         : st == HS_DOUSE      ? "DOUSED"   : st == HS_INCINERATE ? "BURNING" : "RESET";
}
static void statePill(int st, uint32_t now) {
    bool ok = (st == HS_CONTAINED || st == HS_RESET);
    int w = 108, x = W - w - PAD + g_ox, y = 8 + g_oy;
    uint16_t c = ok ? pal.edge : ((now / 320) & 1 ? pal.hot : pal.panel);
    lcd.fillRoundRect(x, y, w, 24, 3, c);
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextColor(ok ? pal.bg : pal.fg, c);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.drawString(pillText(st), x + w / 2, y + 13);
    lcd.setTextDatum(textdatum_t::top_left);
}
static void drawChrome(int st) {
    setPalette(st);
    lcd.fillScreen(pal.bg);
    lcd.fillRect(g_ox, g_oy, W, HDR_H, pal.panel);
    lcd.drawFastHLine(g_ox, HDR_H + g_oy, W, pal.fg);
    trefoil(22 + g_ox, 20 + g_oy, 13, pal.hot);
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextColor(pal.fg, pal.panel);
    lcd.setCursor(44 + g_ox, 4 + g_oy);  lcd.print("DARROW CHEMICAL CO.");
    lcd.setFont(&fonts::Font0);
    lcd.setTextColor(pal.dim, pal.panel);
    lcd.setCursor(44 + g_ox, 26 + g_oy); lcd.print("2-4-5 TRIOXIN  /  U.S. ARMY PROPERTY  /  SITE-D");

    panel(COL_L, ST_Y, COL_W, ST_H, "CONTAINMENT STATUS");
    panel(COL_L, PP_Y, COL_W, PP_H, "ATMOSPHERIC 2-4-5 TRIOXIN");
    panel(COL_L, IN_Y, COL_W, IN_H, "CONTAINMENT FIELD INTEGRITY");
    panel(COL_R, EK_Y, COL_W, EK_H, "SPECIMEN 04 - CEREBRAL ACTIVITY");
    panel(COL_R, LG_Y, COL_W, LG_H, "TELEMETRY");

    ekg.deleteSprite();
    ekg.createSprite(EKG_W, EKG_H);
    ekg.fillSprite(pal.bg);
    s_lastState = st;
}

// ── DOUSE / INCINERATE — full-screen takeover events (2026-09-21, Joe) ─────
// Redesigned per Joe's feedback: the normal medical-monitor layout (PPM /
// EKG / containment-integrity panels) doesn't make sense once the specimen
// is actively being doused or burned — these two states get their own
// completely different full-screen compositions instead of a recolored
// version of the standard chrome. No pixel-orbit burn-in shift here either —
// both are short, active-show states, never a long idle hold.
static void drawDouseScreen(uint32_t now) {
    static uint32_t s_entryMs = 0;
    if (s_lastState != HS_DOUSE) s_entryMs = now;   // first frame of this state

    // deep amber background — slow breathe (2026-09-21: calmed down from a
    // fast 700ms pulse per Joe, "less flashing or flickering")
    float bpulse = 0.5f + 0.5f * sinf(now / 2800.0f);
    uint8_t bgR = (uint8_t)(30 + 15 * bpulse), bgG = (uint8_t)(16 + 8 * bpulse);
    lcd.fillScreen(lcd.color565(bgR, bgG, 0));

    // falling accelerant rain — amber streaks
    struct Drop { int16_t x, y; uint8_t speed, len; };
    static Drop d[36];
    static bool inited = false;
    if (!inited) {
        for (auto& p : d) { p.x = esp_random() % W; p.y = esp_random() % H;
                             p.speed = 4 + esp_random() % 6; p.len = 10 + esp_random() % 14; }
        inited = true;
    }
    for (auto& p : d) {
        lcd.drawFastVLine(p.x, p.y, p.len, lcd.color565(255, 180, 40));
        p.y += p.speed;
        if (p.y > H) { p.y = -p.len; p.x = esp_random() % W; }
    }

    lcd.setTextDatum(textdatum_t::middle_center);
    trefoil(W / 2, 56, 34, lcd.color565(255, 170, 30));

    lcd.setFont(&fonts::FreeSansBold18pt7b);
    lcd.setTextColor(lcd.color565(255, 235, 190), lcd.color565(bgR, bgG, 0));
    lcd.drawString("EMERGENCY", W / 2, 122);
    lcd.drawString("CREMATION PROTOCOL", W / 2, 158);

    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextColor(lcd.color565(255, 200, 120), lcd.color565(bgR, bgG, 0));
    lcd.drawString("ACCELERANT DEPLOYED - SECTOR D", W / 2, 192);

    // fuel saturation bar — climbs over the ~10 s window this state is typically held
    float sat = constrain((now - s_entryMs) / 9000.0f, 0.f, 1.f);
    int bx = 60, by = 226, bw = W - 120, bh = 22;
    lcd.drawRect(bx, by, bw, bh, lcd.color565(255, 170, 30));
    lcd.fillRect(bx + 2, by + 2, (int)((bw - 4) * sat), bh - 4, lcd.color565(255, 140, 0));
    char satb[28]; snprintf(satb, sizeof(satb), "CREMATION FUEL %d%%", (int)(sat * 100));
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(lcd.color565(20, 10, 0));   // single-arg = transparent bg, sits on the bar
    lcd.drawString(satb, W / 2, by + bh / 2 + 1);

    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextColor(lcd.color565(255, 210, 150), lcd.color565(bgR, bgG, 0));
    lcd.drawString("AWAITING IGNITION AUTHORIZATION", W / 2, 278);
    lcd.setTextDatum(textdatum_t::top_left);
    s_lastState = HS_DOUSE;
}

static void drawIncinerateScreen(uint32_t now) {
    // SOLID red background (2026-09-21, Joe: "less flashing or flickering...
    // can you do red here?") — a slow, gentle breathe instead of the earlier
    // fast random-walk flicker, and pure red (no green/blue muddying it into
    // brown/orange).
    float p = 0.5f + 0.5f * sinf(now / 2500.0f);   // ~5 s slow breathe
    uint8_t bgR = (uint8_t)(150 + 40 * p);
    lcd.fillScreen(lcd.color565(bgR, 0, 0));

    // rising embers — smooth continuous motion, not a flicker
    struct Ember { int16_t x, y; uint8_t speed, size; };
    static Ember e[30];
    static bool inited = false;
    if (!inited) {
        for (auto& p2 : e) { p2.x = esp_random() % W; p2.y = esp_random() % H;
                              p2.speed = 2 + esp_random() % 5; p2.size = 1 + esp_random() % 3; }
        inited = true;
    }
    for (auto& p2 : e) {
        uint8_t g = 120 + esp_random() % 100;
        lcd.fillCircle(p2.x, p2.y, p2.size, lcd.color565(255, g, 0));
        p2.y -= p2.speed;
        p2.x += (int16_t)(esp_random() % 3) - 1;
        if (p2.y < 0) { p2.y = H; p2.x = esp_random() % W; }
    }

    // headline — static placement, no jitter
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setFont(&fonts::FreeSansBold18pt7b);
    lcd.setTextColor(lcd.color565(255, 245, 210), lcd.color565(bgR, 0, 0));
    lcd.drawString("SPECIMEN 04", W / 2, 110);
    lcd.setTextColor(lcd.color565(255, 210, 60), lcd.color565(bgR, 0, 0));
    lcd.drawString("CREMATION ACTIVE", W / 2, 150);

    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextColor(lcd.color565(255, 180, 120), lcd.color565(bgR, 0, 0));
    lcd.drawString("EMERGENCY INCINERATION - PURGE IN PROGRESS", W / 2, 184);

    // structural integrity: pegged at zero — steady, no blink
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(lcd.color565(255, 255, 255), lcd.color565(bgR, 0, 0));
    lcd.drawString("STRUCTURAL INTEGRITY: 0%", W / 2, 224);

    char tb[24]; snprintf(tb, sizeof(tb), "CHAMBER TEMP %d C", 900 + (int)(esp_random() % 300));
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(lcd.color565(255, 220, 180), lcd.color565(bgR, 0, 0));
    lcd.drawString(tb, W / 2, 252);

    lcd.setFont(&fonts::FreeSansBold12pt7b);
    lcd.setTextColor(lcd.color565(255, 255, 255), lcd.color565(bgR, 0, 0));
    lcd.drawString("** EMERGENCY **", W / 2, 288);
    lcd.setTextDatum(textdatum_t::top_left);
    s_lastState = HS_INCINERATE;
}

// ── dynamic ────────────────────────────────────────────────────────────────
static void drawStatus(int st, uint32_t now) {
    body(COL_L, ST_Y, COL_W, ST_H);
    bool blink = (now / 340) & 1;
    // Indexed by st (0..6) — must stay in lockstep with the HS_* enum order
    // in dmx_state.h (HS_CONTAINED..HS_RESET).
    struct { const char* big; const char* sub; } T[7] = {
        { "CONTAINED",   "SPECIMEN 04 SECURE" },
        { "UNSTABLE",    "SECURE CONTAINER / DO NOT TAMPER" },
        { "BREACH",      "TRIOXIN-245 RELEASE" },
        { "SPECIMEN LOOSE", "SYSTEM BREACH" },
        { "DOUSING SPECIMEN", "ACCELERANT DEPLOYED" },
        { "INCINERATION", "SPECIMEN 04 - PURGE ACTIVE" },
        { "RECONTAINMENT", "PURGE CYCLE - STANDBY" },
    };
    lcd.setFont(strlen(T[st].big) > 10 ? &fonts::FreeSansBold9pt7b : &fonts::FreeSansBold12pt7b);
    bool crit = (st != HS_CONTAINED && st != HS_RESET);
    lcd.setTextColor(crit && blink ? pal.hot : pal.fg, pal.bg);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.drawString(T[st].big, COL_L + COL_W / 2, ST_Y + 38);
    lcd.setFont(&fonts::Font0);
    lcd.setTextColor(pal.dim, pal.bg);
    lcd.drawString(T[st].sub, COL_L + COL_W / 2, ST_Y + 62);
    lcd.setTextDatum(textdatum_t::top_left);
}

static void drawPPM(int st, float esc, uint32_t now) {
    body(COL_L, PP_Y, COL_W, PP_H);
    char num[8];
    bool over = false, brk = (st == HS_BROKEN);
    if (brk) {
        const char* g[] = { "----", "ERR", "????", "1EE3", "0x7F", "----" };
        strcpy(num, g[(now / 260) % 6]);
    } else {
        int ppm = st == HS_CONTAINED  ? (int)(38 + 8 * sinf(now / 900.0f))
                : st == HS_RESET      ? (int)(60 + esc * 900)
                : st == HS_WARNING    ? (int)(180 + esc * 700 + 20 * sinf(now / 220.0f))
                : st == HS_DOUSE      ? (int)(700 + 250 * sinf(now / 180.0f))       // accelerant vapor climbing
                : st == HS_INCINERATE ? 1800 + (int)(esp_random() % 700)            // combustion byproducts, pegged
                :                      1400 + (int)(esp_random() % 500);   // glass break
        over = ppm > 1000;
        snprintf(num, sizeof(num), "%d", ppm > 9999 ? 9999 : ppm);
    }
    lcd.setFont(&fonts::Font6);
    lcd.setTextColor((over || brk) ? pal.hot : pal.fg, pal.bg);
    lcd.setCursor(COL_L + 8, PP_Y + 22); lcd.print(num);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(pal.dim, pal.bg);
    lcd.setCursor(COL_L + 150, PP_Y + 30); lcd.print("PPM");
    if ((over || st == HS_GLASSBREAK || st == HS_INCINERATE) && ((now / 200) & 1)) {
        lcd.setTextColor(pal.hot, pal.bg);
        lcd.setCursor(COL_L + 150, PP_Y + 48); lcd.print("OVERRANGE");
    }
    int bx = COL_L + 10, by = PP_Y + PP_H - 14, bw = COL_W - 20, bh = 8;
    lcd.drawRect(bx, by, bw, bh, pal.edge);
    float frac = brk ? (0.4f + 0.6f * ((now / 90) % 2))
               : st == HS_CONTAINED ? 0.03f
               : st == HS_RESET ? (0.9f - esc * 0.85f)
               : st == HS_WARNING ? (0.15f + esc * 0.55f)
               : st == HS_DOUSE ? (0.55f + 0.2f * sinf(now / 180.0f)) : 1.0f;
    int f = (int)(constrain(frac, 0.f, 1.f) * (bw - 2));
    lcd.fillRect(bx + 1, by + 1, f, bh - 2, (over || brk) ? pal.hot : pal.fg);
    lcd.drawFastVLine(bx + (int)(bw * 0.71f), by - 3, bh + 6, pal.hot);
}

static void drawIntegrity(int st, float esc, uint32_t now) {
    body(COL_L, IN_Y, COL_W, IN_H);
    lcd.setFont(&fonts::Font4);
    char b[8];
    int pct;
    if (st == HS_BROKEN)       { pct = -1; strcpy(b, ((now / 300) & 1) ? " --%" : "  ?%"); }
    else if (st == HS_CONTAINED) { pct = 100; strcpy(b, "100%"); }
    else if (st == HS_RESET)   { pct = (int)(40 + (1 - esc) * 58); snprintf(b, sizeof(b), "%3d%%", pct); }
    else if (st == HS_WARNING) { pct = (int)(90 - esc * 55);       snprintf(b, sizeof(b), "%3d%%", pct); }
    else if (st == HS_DOUSE)      { pct = 2; strcpy(b, "  2%"); }
    else if (st == HS_INCINERATE) { pct = 0; strcpy(b, "  0%"); }
    else                       { pct = 4; strcpy(b, "  4%"); }
    lcd.setTextColor((pct < 0 || pct <= 60) ? pal.hot : pal.fg, pal.bg);
    lcd.setCursor(COL_L + 8, IN_Y + 20); lcd.print(b);
    int sx = COL_L + 84, sw = COL_W - 84 - 10, n = 18, gw = sw / n;
    for (int i = 0; i < n; i++) {
        bool lit = (pct >= 0) && ((i * 100 / n) < pct);
        lcd.fillRect(sx + i * gw, IN_Y + 22, gw - 2, 14,
                     lit ? (pct <= 60 ? pal.hot : pal.fg) : pal.panel);
    }
    lcd.setFont(&fonts::Font0);
    lcd.setTextColor(pal.dim, pal.bg);
    lcd.setCursor(COL_L + 8, IN_Y + IN_H - 14);
    // Indexed by st (0..6), same order as the HS_* enum — see drawStatus().
    const char* sub[7] = { "VAPOR SEAL: HOLDING", "SEAL DEGRADING - ESTOP READY",
                           "SEAL FAILURE - GAS RELEASE", "SEAL: FAILED   SENSORS: LOST",
                           "ACCELERANT SATURATING SPECIMEN", "SEAL: CONSUMED - FULL BURN",
                           "SEAL RE-PRESSURIZING" };
    lcd.print(sub[st]);
}

static void drawFooter(int st, uint32_t now) {
    uint32_t t = (now - s_bootMs) / 1000;
    int fy = 302 + g_oy;
    lcd.fillRect(g_ox, fy, W, 18, pal.panel);
    lcd.drawFastHLine(g_ox, fy, W, pal.edge);
    lcd.setFont(&fonts::Font0);
    lcd.setTextColor(pal.fg, pal.panel);
    const char* link = st == HS_CONTAINED ? "STATUS OK" : st == HS_RESET ? "RECOVERING"
                     : st == HS_BROKEN ? "** NO SIGNAL **"
                     : st == HS_INCINERATE ? "** FIRE **" : "** ALARM **";
    char b[72]; snprintf(b, sizeof(b), "T+ %02lu:%02lu:%02lu     BUS 27.%luV     %s     CH11",
                         t / 3600, (t / 60) % 60, t % 60, (now / 120) % 9, link);
    lcd.setCursor(8 + g_ox, fy + 4); lcd.print(b);
}

// ── STANDBY screensaver — dim wandering card for long CONTAINED holds ───────
static void drawIdle(uint32_t now) {
    static uint32_t blink = 0, clk = 0;
    bool moved = false;
    if (s_idleMoveAt == 0 || now - s_idleMoveAt > 15000) {
        lcd.fillRect(s_idleX - 6, s_idleY - 6, 236, 84, TFT_BLACK);   // erase old card
        s_idleX = 14 + (int)(esp_random() % (W - 250));
        s_idleY = 22 + (int)(esp_random() % (H - 110));
        s_idleMoveAt = now;
        moved = true;
    }
    int x = s_idleX, y = s_idleY;
    if (moved) {
        trefoil(x + 14, y + 16, 12, 0x05C0);
        lcd.setFont(&fonts::FreeSansBold12pt7b);
        lcd.setTextColor(0x2FEC, TFT_BLACK);
        lcd.setCursor(x + 38, y + 3); lcd.print("CONTAINED");
        lcd.setFont(&fonts::Font0);
        lcd.setTextColor(0x0540, TFT_BLACK);
        lcd.setCursor(x, y + 34); lcd.print("SITE-D  SECURE   ATMOSPHERE NOMINAL");
        lcd.setCursor(x, y + 48); lcd.print("DARROW CHEMICAL CO.  /  2-4-5 TRIOXIN  /  STANDBY");
        clk = 0;
    }
    if (now - blink > 1100) {          // slow "alive" pulse
        blink = now;
        static bool on = false; on = !on;
        lcd.fillCircle(x + 220, y + 6, 3, on ? 0x2FEC : TFT_BLACK);
    }
    if (now - clk > 1000) {            // mission clock, in place
        clk = now;
        uint32_t t = (now - s_bootMs) / 1000;
        char b[24]; snprintf(b, sizeof(b), "T+ %02lu:%02lu:%02lu", t / 3600, (t / 60) % 60, t % 60);
        lcd.setFont(&fonts::Font0);
        lcd.setTextColor(0x0540, TFT_BLACK);
        lcd.fillRect(x + 120, y + 18, 90, 10, TFT_BLACK);
        lcd.setCursor(x + 120, y + 18); lcd.print(b);
    }
}

// ── touch override (2026-09-21) ──────────────────────────────────────────────
static ZombieFireFn    s_zombieFire    = nullptr;

void hud_set_zombie_callbacks(ZombieFireFn fire) { s_zombieFire = fire; }

// REMOVED 2026-09-20: the custom 4-corner lcd.setTouchCalibrate() routine
// that used to live here. Root-caused as buggy — Panel_Device::setCalibrate()
// scores captured corners against the panel's native PRE-rotation width/
// height, while the crosshairs it drew were in the already-rotated landscape
// frame, a real coordinate-frame mismatch. Touch calibration is now the
// plain x_min/x_max/y_min/y_max + offset_rotation in lgfx_conf.h, hand-tuned
// from real bench data (see docs/zombie-container.md "Touch calibration").

void hud_set_touch_rotation(uint8_t r) {
    auto t = lcd.touch();
    if (!t) { Serial.println(F("[touch] no touch driver attached")); return; }
    auto cfg = t->config();
    cfg.offset_rotation = r & 7;
    t->config(cfg);   // LGFXBase::convertRawXY() re-reads this live, no re-init needed
    Serial.printf("[touch] offset_rotation = %u\n", cfg.offset_rotation);
}

// ── manual override — ONE full-screen button, no split zones anywhere ─────
// 2026-09-21 redesign (Joe, after the 2026-09-20 touch-calibration session):
// the whole panel is a single tap target on both taps — open, then confirm —
// so there is no boundary to miss and nothing to mis-aim at. No ARM/DISARM
// toggle and no Singing Pumpkins jukebox here anymore (dropped from touch;
// arm/disarm remain serial-only in main.cpp). E-STOP was already never on
// touch (2026-09-19, Joe) — once the relay pulses the PicoDMX trigger, the
// PicoDMX runs its whole ~184 s show with no wire back to this panel to stop it.
static void drawOverrideStatus(bool running) {
    lcd.fillRect(0, H - 30, W, 30, TFT_BLACK);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    if (running) {
        uint32_t el = dmx_elapsed_ms() / 1000;
        char b[40]; snprintf(b, sizeof(b), "SHOW RUNNING  T+ %02lu:%02lu / 03:04",
                             (unsigned long)(el / 60), (unsigned long)(el % 60));
        lcd.setTextColor(0xFFE0, TFT_BLACK);
        lcd.drawString(b, W / 2, H - 15);
    } else {
        char b[40]; snprintf(b, sizeof(b), "no tap in %lus -> back to monitor",
                             (unsigned long)(MENU_TIMEOUT_MS / 1000));
        lcd.setTextColor(0x8410, TFT_BLACK);
        lcd.drawString(b, W / 2, H - 15);
    }
    lcd.setTextColor(0xFFFF, TFT_BLACK);
    lcd.setTextDatum(textdatum_t::top_left);
}

static void drawOverride() {
    lcd.fillScreen(0x8200);   // amber — distinct from every HUD state color
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(0xFFFF, 0x8200);
    lcd.setFont(&fonts::FreeSansBold18pt7b);
    lcd.drawString("TAP TO FIRE", W / 2, H / 2 - 30);
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.drawString("MANUAL OVERRIDE - ZOMBIE CONTAINER", W / 2, H / 2 + 10);
    drawOverrideStatus(false);
    lcd.setTextColor(0xFFFF, TFT_BLACK);
    lcd.setTextDatum(textdatum_t::top_left);
}

static void hudOverrideTick(bool tapped, uint32_t now) {
    static uint32_t s_statusNext = 0;
    if (s_dirty) { drawOverride(); s_dirty = false; s_statusNext = 0; }
    if (dmx_segment_running() && now >= s_statusNext) {
        drawOverrideStatus(true); s_statusNext = now + 500;
    }
    if (!tapped) return;
    // Whole screen is the button — any release inside the panel fires.
    // Still can't re-fire mid-show: dmx_segment_running() gates it here AND
    // inside fireZombie() itself (main.cpp), belt-and-suspenders.
    if (!dmx_segment_running() && s_zombieFire) {
        s_zombieFire();
        s_mode = MODE_HUD; s_lastState = -99;   // jump back to the monitor to watch it run
    }
}

// ── Frozen override test (2026-09-21, bench-only) ───────────────────────────
// The exact real override screen, but a tap never fires the relay — it just
// logs. The whole screen is one region now, so there's nothing left to
// classify; this just confirms taps are being read at all and where.
static int s_freezeHits = 0;

static void drawFreezeTest() {
    drawOverride();
    lcd.setTextDatum(textdatum_t::top_left);
    lcd.setTextColor(0xFFFF, TFT_BLACK);
    lcd.setFont(&fonts::Font2);
    char buf[24];
    snprintf(buf, sizeof(buf), "FREEZE  hits: %d", s_freezeHits);
    lcd.drawString(buf, 6, 6);
}

static void hudFreezeTick(bool tapped, int32_t tx, int32_t ty) {
    if (s_dirty) { drawFreezeTest(); s_dirty = false; }
    if (!tapped) return;
    s_freezeHits++;
    Serial.printf("[freeze] tap at screen x=%ld y=%ld  ->  OVERRIDE (%d total)\n",
                  (long)tx, (long)ty, s_freezeHits);
    s_dirty = true;   // just redraws the hit count — mode never changes
}

void hud_start_freezetest() {
    s_freezeHits = 0;
    s_mode = MODE_FREEZE;
    s_dirty = true;
}

// ── public ─────────────────────────────────────────────────────────────────
void hud_setup() {
    lcd.init();
    lcd.setRotation(3);          // landscape, flipped from 1

    // Touch calibration lives in lgfx_conf.h as plain x_min/x_max/y_min/y_max
    // + offset_rotation (2026-09-20) — NOT lcd.setTouchCalibrate(). The
    // 4-corner setTouchCalibrate() affine path (2026-09-19) had a real
    // coordinate-frame bug: Panel_Device::setCalibrate() scores captured
    // corners against the panel's native pre-rotation width/height
    // (320x480), while the crosshairs here are drawn in the already-rotated
    // landscape frame (480x320) — a genuine width/height mismatch, which is
    // why that flash made things worse instead of better. Going back to the
    // simple linear min/max scale, which was already bench-confirmed correct
    // in formula (2026-09-19 Bug #2) — it just needed the right constants.

    lcd.setBrightness(165);
    lcd.setTextWrap(false);
    lcd.fillScreen(TFT_BLACK);
    s_bootMs = millis();
    s_inited = true;
    for (int i = 0; i < LOGN; i++) s_log[i] = "";
    logPush("DARROW CHEMICAL CO.  MONITOR v2.45");
    logPush("LINK ESTABLISHED - STANDING BY");
}

void hud_force_idle(bool on) { s_forceIdle = on; }   // bench: preview the STANDBY screensaver

void hud_set_enabled(bool on) {
    s_enabled = on;
    if (!s_inited) return;
    if (on) s_lastState = -99;               // force a full redraw + brightness on next tick
    else    lcd.setBrightness(0);
}

// Touch UI RE-ENABLED 2026-09-21 with the single-button redesign below —
// see hud.h for the flow. The relay/ESP-NOW auto-fire path (ROTLD theme-lock,
// armed by default) needs zero touch input and is unaffected either way; this
// is purely a manual fallback. Flip to false to go back to display-only.
static const bool TOUCH_UI_ENABLED = true;

void hud_tick() {
    if (!s_inited || !s_enabled) return;
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 45) return;
    last = now;

    if (TOUCH_UI_ENABLED) {
    // ── touch — RELEASE-detected tap, drives the control menu ──────────────
    // 2026-09-20 (Joe, live bench session): this used to fire on the DOWN
    // edge, at whatever position the very first noisy contact sample landed
    // on. On a resistive panel a finger lands imprecisely and slides into
    // place before lifting — so taps kept registering at the approach point,
    // not the intended button (BACK dragging into PLAY, ZOMBIE dragging into
    // PUMPKINS, list scrolls firing multiple times during one drag). Standard
    // touchscreen model instead: track position while held, commit the tap
    // on release using the LAST held position — wherever the finger actually
    // ends up when it lifts.
    static int32_t s_lastTx = 0, s_lastTy = 0;
    int32_t tx = 0, ty = 0;
    bool down = lcd.getTouch(&tx, &ty);
    if (down) {
        // Clamp (2026-09-20) — the linear x_min/x_max/y_min/y_max scale can
        // overscoot slightly past the true screen edges (real bench data:
        // bottom-edge presses computing to y=320-323, one past the valid
        // 0-319 range), which made every hit-zone miss and register as a
        // dead zone right at the edges — exactly where the BACK strip lives.
        if (tx < 0) tx = 0; else if (tx > W - 1) tx = W - 1;
        if (ty < 0) ty = 0; else if (ty > H - 1) ty = H - 1;
        s_lastTx = tx; s_lastTy = ty;
    }
    // DEBUG (2026-09-19): print every touch read — both the final screen-space
    // x/y (post rotation+calibration) AND the RAW sensor x/y (pre-calibration).
    // Real bench data showed the final x never exceeds ~230 (out of 479) no
    // matter how far right the physical press is — the guessed x_max=3900 in
    // lgfx_conf.h is higher than this panel's real raw sensor range, so the
    // linear scale never reaches the upper half of screen X. The RAW values
    // here are what's needed to compute the real x_min/x_max/y_min/y_max.
    // Remove once real calibration constants are confirmed working.
    static bool s_dbgWasDown = false;
    static uint32_t s_dbgLast = 0;
    if (down && (!s_dbgWasDown || now - s_dbgLast > 200)) {
        lgfx::touch_point_t rawTp;
        auto t = lcd.touch();
        bool gotRaw = t && t->getTouchRaw(&rawTp, 1);
        Serial.printf("[touch] DOWN x=%ld y=%ld   RAW x=%d y=%d z=%d\n",
                      (long)tx, (long)ty,
                      gotRaw ? rawTp.x : -1, gotRaw ? rawTp.y : -1, gotRaw ? rawTp.size : -1);
        s_dbgLast = now;
    } else if (!down && s_dbgWasDown) {
        Serial.println("[touch] UP");
    }
    s_dbgWasDown = down;
    bool tapped = !down && s_touchDown;   // release edge
    if (tapped) { tx = s_lastTx; ty = s_lastTy; }   // wherever the finger last was
    s_touchDown = down;

    if (s_mode == MODE_FREEZE) { hudFreezeTick(tapped, tx, ty); return; }

    // ── show-in-progress LOCKOUT (Joe, 2026-09-21) ─────────────────────────
    // While the DMX segment is running, the override screen cannot be opened
    // at all — any tap is ignored outright, and if it was somehow already
    // open (e.g. the auto-fire path started a show while the override screen
    // was sitting open), it's forced straight back to the live phase-reactive
    // monitor so the panel always shows the correct state in sync with the
    // running show.
    if (dmx_segment_running()) {
        if (s_mode == MODE_OVERRIDE) { s_mode = MODE_HUD; s_lastState = -99; }
    } else if (s_mode == MODE_OVERRIDE) {
        if (tapped) s_menuTouchAt = now;
        if (now - s_menuTouchAt > MENU_TIMEOUT_MS) {
            s_mode = MODE_HUD; s_lastState = -99;   // no touch for a while -> back to CONTAINED, no fire
        } else {
            hudOverrideTick(tapped, now);
            return;
        }
    } else if (tapped && (s_mode == MODE_HUD || s_mode == MODE_IDLE)) {
        s_mode = MODE_OVERRIDE; s_menuTouchAt = now; s_dirty = true;
        return;
    }
    }   // TOUCH_UI_ENABLED

    int st = dmx_show_state();
    float esc = dmx_escalation();

    // ── STANDBY screensaver gate — bench-preview only (2026-09-19), see the
    // file header: the animated CONTAINED HUD is the permanent passive display
    // now, so this never fires on its own. `s_forceIdle` (the `idle` serial
    // cmd) still previews the old design on request.
    bool wantIdle = s_forceIdle;
    if (wantIdle && s_mode != MODE_IDLE) {
        s_mode = MODE_IDLE; s_idleMoveAt = 0;
        setPalette(HS_CONTAINED);
        lcd.fillScreen(TFT_BLACK);
        applyBrightness(st);
    } else if (!wantIdle && s_mode == MODE_IDLE) {
        s_mode = MODE_HUD; s_lastState = -99;
        applyBrightness(st);
    }
    if (s_mode == MODE_IDLE) { drawIdle(now); return; }

    // ── pixel orbit (burn-in) — force a full repaint at the new offset ────
    if (now - s_orbAt > ORBIT_MS) {
        s_orbAt = now;
        s_orbIx = (uint8_t)((s_orbIx + 1) % 8);
        static const int8_t ORB[8][2] =
            { {-6,-4}, {-2,-6}, { 3,-6}, { 6,-2}, { 6, 3}, { 2, 6}, {-3, 6}, {-6, 2} };
        g_ox = ORB[s_orbIx][0]; g_oy = ORB[s_orbIx][1];
        s_lastState = -99;
    }

    // DOUSE / INCINERATE — full-screen takeover, bypasses the normal
    // chrome/panel pipeline entirely (see drawDouseScreen/drawIncinerateScreen).
    if (st == HS_DOUSE)      { applyBrightness(st); drawDouseScreen(now);      return; }
    if (st == HS_INCINERATE) { applyBrightness(st); drawIncinerateScreen(now); return; }

    if (st != s_lastState) { drawChrome(st); applyBrightness(st); }

    statePill(st, now);
    drawStatus(st, now);
    drawPPM(st, esc, now);
    ekgAdvance(st);
    drawIntegrity(st, esc, now);
    logTick(st, now);
    drawLog(st);
    drawFooter(st, now);
}
