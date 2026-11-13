// =============================================================================
// ShowFollower.h — hardened "follow the Scarecrow" cue logic for theme-bed props
// =============================================================================
// Used by props/spooky-tree and props/pumpkin-men (2026-09-24,
// show-sync-orchestrator). Pure logic: no Arduino, no radio, no MP3 calls, so
// it is host-tested (props/_tests/test_show_follower.cpp) and both nodes run
// the identical decision code. The node feeds it frames, end-of-track events
// and a periodic tick; it answers with ONE action (play theme X / go back to
// idle mode / nothing) plus a reason string the node logs.
//
// The Scarecrow is the show controller. Per visit it sends (all 3x bursts):
//   PRE-SHOW   SET_THEME(theme) + SET_MODE + SHOW_STATE(SHOW_NARRATOR)
//   MAIN-SHOW  START_SCENE(theme) ... SHOW_STATE(SHOW_ACTIVE)
//   +210 s     SHOW_STATE(SHOW_OFF), then SHOW_STATE(SHOW_IDLE)
//   always     CMD_HEARTBEAT every 10 s (liveness only)
// Older Scarecrow firmware (pre-711f9e2) sent SHOW_STATE(SHOW_IDLE) as its
// 10 s heartbeat, so OFF/IDLE is only honoured late in a run (offMinRunMs).
//
// Rules (each one has a test):
//   R1 Only frames from the Scarecrow drive the show; anything else is counted
//      and ignored. Invalid theme values are ignored (theme stays as it was).
//   R2 The Scarecrow's theme is the theme. SET_THEME and START_SCENE latch it.
//      If the theme changes for the run that is playing, the track is replaced.
//   R3 SHOW_NARRATOR from a NEW session = a new visit. If a theme track is
//      still playing (previous visit), it stops and the prop goes back to its
//      idle mode under the narrators (cfg.stopOnNewNarrator).
//      OFF on Tree / Pumpkin Men / Patch L/R since 2026-10-27: a Scarecrow
//      resync burst looks the same on the wire and cut a playing theme.
//   R4 SHOW_ACTIVE starts the latched theme track, replacing whatever plays.
//      Repeats of the same session within dupWindowMs of the first are the 3x
//      burst: ignored, even if the run has since been stopped.
//      Only a theme latched FOR THAT SESSION plays (2026-10-26: the Tree played
//      the previous visit's ROTLD while Pumpkin Men + door played Mutant; a
//      walkway-only main show carries its theme in START_SCENE alone). No
//      theme for the session -> no guess: a stale track stops, the prop sits
//      this show out in its idle mode, WARN. Silence beats the wrong theme.
//      (Not done: stopping a run when the heartbeat's session moves on. An
//      encore gets a new session with no music, so that would cut the theme.)
//   R5 The track plays to its end. End-of-track sooner than earlyEotMs after
//      PLAY is treated as a stale event from the replaced track and ignored.
//   R6 SHOW_OFF / SHOW_IDLE end the run only after offMinRunMs of playing.
//   R7 Fail-safe: no end-of-track and no OFF by showActiveMs + deadlineMarginMs
//      -> back to idle mode anyway.
//   R8 A visit that never reaches SHOW_ACTIVE times out (narratorTimeoutMs).
//   R10 MP3 start failure (onPlayFailed) ends the run at once -> idle mode, WARN.
//   R9 Leader liveness: any valid Scarecrow frame (incl. HEARTBEAT) refreshes
//      it; leaderSilent() reports > leaderSilentMs of nothing. Never stops a
//      track by itself — the show goes on, R7 bounds it.
// Every decision returns a reason; the node prints all of them.
// =============================================================================
#pragma once
#include <stdint.h>

namespace showfollow {

// Wire values, mirrored from ProjectConstants.h so this header compiles on the
// host. Firmware checks they still match (static_asserts in each node).
enum : uint8_t {
    W_CMD_START_SCENE = 0x01,
    W_CMD_SET_THEME   = 0x02,
    W_CMD_STOP        = 0x07,
    W_CMD_SHOW_STATE  = 0x28,
    W_CMD_HEARTBEAT   = 0x40,
    W_NODE_SCARECROW  = 0x06,
    W_SHOW_IDLE       = 0,
    W_SHOW_PENDING    = 1,
    W_SHOW_NARRATOR   = 2,
    W_SHOW_ACTIVE     = 3,
    W_SHOW_OFF        = 4,
    W_THEME_COUNT     = 3,   // CORN=0, ROTLD=1, MUTANT_PUMPKINS=2
};

struct Config {
    uint32_t showActiveMs      = 210000;  // SHOW_ACTIVE_MS
    uint32_t deadlineMarginMs  = 20000;   // R7 backstop = showActiveMs + this
    uint32_t dupWindowMs       = 10000;   // R4
    uint32_t earlyEotMs        = 3000;    // R5
    uint32_t offMinRunMs       = 180000;  // R6
    uint32_t narratorTimeoutMs = 600000;  // R8
    uint32_t leaderSilentMs    = 35000;   // R9 (3.5 missed heartbeats)
    uint32_t expectedTrackMs[W_THEME_COUNT] = {180000, 180000, 180000};
    uint32_t durationWarnMs    = 10000;   // |played - expected| beyond this -> WARN
    bool     stopOnNewNarrator = true;    // R3
};

enum class Phase : uint8_t { IDLE, VISIT, PLAYING };
enum class Act   : uint8_t { NONE, PLAY, TO_IDLE };

struct Out {
    Act         act   = Act::NONE;
    uint8_t     theme = 0;
    const char* why   = nullptr;   // non-null -> node logs it
    bool        warn  = false;     // log as WARN
};

struct Stats {
    uint16_t plays = 0, replaces = 0, dupIgnored = 0, badSender = 0, badTheme = 0;
    uint16_t earlyEotIgnored = 0, earlyOffIgnored = 0, deadlineHits = 0;
    uint16_t themeUnconfirmed = 0, durationWarns = 0, visitTimeouts = 0, playFails = 0;
};

class Follower {
public:
    explicit Follower(const Config& c = Config()) : cfg_(c) {}

    // ── inputs ───────────────────────────────────────────────────────────────
    Out onFrame(uint8_t cmd, uint8_t sender, uint32_t session, uint32_t payloadA, uint32_t now) {
        const bool showCmd = cmd == W_CMD_SET_THEME || cmd == W_CMD_START_SCENE ||
                             cmd == W_CMD_SHOW_STATE || cmd == W_CMD_HEARTBEAT;
        if (!showCmd) return Out();                      // not ours (STOP etc. handled by node)
        if (sender != W_NODE_SCARECROW) {                // R1
            stats_.badSender++;
            return note("show cue from a non-Scarecrow sender ignored", true);
        }
        leaderRx_ = now; everHeardLeader_ = true;        // R9

        switch (cmd) {
        case W_CMD_HEARTBEAT:
            return Out();
        case W_CMD_SET_THEME:
        case W_CMD_START_SCENE:
            return latchTheme(payloadA, session, now);
        default:  // W_CMD_SHOW_STATE
            switch (payloadA) {
            case W_SHOW_NARRATOR: return onNarrator(session, now);
            case W_SHOW_ACTIVE:   return onActive(session, now);
            case W_SHOW_OFF:
            case W_SHOW_IDLE:     return onOff(now);
            default:              return Out();          // PENDING etc.
            }
        }
    }

    // Node calls this when the MP3 player reports end-of-track while a
    // follower run is playing (isPlaying()).
    Out onTrackEnd(uint32_t now) {
        if (phase_ != Phase::PLAYING) return Out();
        uint32_t played = now - runStart_;
        if (played < cfg_.earlyEotMs) {                  // R5
            stats_.earlyEotIgnored++;
            return note("end-of-track right after PLAY ignored (stale, from the replaced track)", true);
        }
        phase_ = Phase::IDLE;
        uint32_t exp = cfg_.expectedTrackMs[runTheme_];
        uint32_t diff = played > exp ? played - exp : exp - played;
        lastPlayedMs_ = played;
        if (diff > cfg_.durationWarnMs) {
            stats_.durationWarns++;
            return make(Act::TO_IDLE, runTheme_,
                        "theme track done, but its length is off the expected (wrong file on SD?)", true);
        }
        return make(Act::TO_IDLE, runTheme_, "theme track done");
    }

    // Node calls this when the MP3 driver could not start the run's track
    // (never confirmed playing, file missing, module reset). No point waiting
    // for the 230 s fail-safe in silence: back to idle mode now, loudly.
    Out onPlayFailed(uint32_t now) {
        (void)now;
        if (phase_ != Phase::PLAYING) return Out();
        phase_ = Phase::IDLE;
        stats_.playFails++;
        return make(Act::TO_IDLE, runTheme_, "theme track did NOT start on the MP3 module -> back to idle mode", true);
    }

    Out tick(uint32_t now) {
        // Signed elapsed: a frame handled after the node took `now` stamps a
        // LATER time, and unsigned now - start wrapped to ~49 days -> instant
        // visit timeout / fail-safe (bench 2026-09-26, Pumpkin Men).
        if (phase_ == Phase::PLAYING &&
            since(now, runStart_) >= (int32_t)(cfg_.showActiveMs + cfg_.deadlineMarginMs)) {   // R7
            phase_ = Phase::IDLE;
            stats_.deadlineHits++;
            return make(Act::TO_IDLE, runTheme_, "FAIL-SAFE deadline: no end-of-track and no SHOW_OFF", true);
        }
        if (phase_ == Phase::VISIT && since(now, visitStart_) >= (int32_t)cfg_.narratorTimeoutMs) {  // R8
            phase_ = Phase::IDLE;
            stats_.visitTimeouts++;
            return note("visit never reached SHOW_ACTIVE, timed out", true);
        }
        bool silent = leaderSilent(now);
        if (silent != leaderWasSilent_) {
            leaderWasSilent_ = silent;
            return note(silent ? "Scarecrow silent (no frames) — continuing on local timing"
                               : "Scarecrow heard again", silent);
        }
        return Out();
    }

    // Node-side hard stop (CMD_STOP, SHOW_UNLOCK, bench stop): forget the run.
    void abort() { phase_ = Phase::IDLE; }

    // ── status ───────────────────────────────────────────────────────────────
    Phase phase() const          { return phase_; }
    bool isPlaying() const       { return phase_ == Phase::PLAYING; }
    bool inVisit() const         { return phase_ == Phase::VISIT; }
    uint8_t theme() const        { return theme_; }
    uint8_t runTheme() const     { return runTheme_; }
    uint32_t runSession() const  { return runSession_; }
    uint32_t runElapsed(uint32_t now) const { return phase_ == Phase::PLAYING ? now - runStart_ : 0; }
    uint32_t lastPlayedMs() const { return lastPlayedMs_; }
    bool leaderSilent(uint32_t now) const {
        return !everHeardLeader_ || since(now, leaderRx_) > (int32_t)cfg_.leaderSilentMs;
    }
    const Stats& stats() const   { return stats_; }
    static const char* phaseName(Phase p) {
        return p == Phase::PLAYING ? "PLAYING" : p == Phase::VISIT ? "VISIT" : "IDLE";
    }

private:
    static int32_t since(uint32_t now, uint32_t t) { return (int32_t)(now - t); }  // < 0 = stamped after now
    Out latchTheme(uint32_t t, uint32_t session, uint32_t now) {
        if (t >= W_THEME_COUNT) {                        // R1
            stats_.badTheme++;
            return note("invalid theme value ignored", true);
        }
        bool changed = (uint8_t)t != theme_;
        theme_ = (uint8_t)t; themeSession_ = session; themeEverSet_ = true;
        // R2: the Scarecrow changed the theme of the run that is playing
        if (phase_ == Phase::PLAYING && session == runSession_ && theme_ != runTheme_) {
            return startRun(session, now, true, "theme changed mid-run -> replacing track");
        }
        return changed ? note("theme latched") : Out();
    }

    Out onNarrator(uint32_t session, uint32_t now) {
        if (phase_ != Phase::IDLE && session == visitSession_) return Out();  // burst dup
        visitSession_ = session; visitStart_ = now;
        if (phase_ == Phase::PLAYING && session != runSession_ && cfg_.stopOnNewNarrator) {  // R3
            phase_ = Phase::VISIT;
            return make(Act::TO_IDLE, runTheme_, "new visit (SHOW_NARRATOR) -> previous theme track stopped");
        }
        if (phase_ == Phase::PLAYING) return note("new visit while playing (kept playing)");
        phase_ = Phase::VISIT;
        return note("visit started (SHOW_NARRATOR)");
    }

    Out onActive(uint32_t session, uint32_t now) {
        // R4 burst dup — checked whether or not a run is still going, so a
        // repeat that lands after a STOP/abort/early end can't restart it.
        if (activeSeen_ && session == lastActiveSession_ && now - lastActiveAt_ < cfg_.dupWindowMs) {
            stats_.dupIgnored++;
            return Out();
        }
        activeSeen_ = true; lastActiveSession_ = session; lastActiveAt_ = now;
        const bool replacing = phase_ == Phase::PLAYING;
        return startRun(session, now, replacing,
                        replacing ? "SHOW_ACTIVE -> replacing the playing track" : "SHOW_ACTIVE -> theme track");
    }

    Out onOff(uint32_t now) {
        if (phase_ == Phase::PLAYING) {
            if (now - runStart_ >= cfg_.offMinRunMs) {   // R6
                phase_ = Phase::IDLE;
                lastPlayedMs_ = now - runStart_;
                return make(Act::TO_IDLE, runTheme_, "SHOW_OFF/IDLE -> show window closed");
            }
            stats_.earlyOffIgnored++;
            return Out();   // early OFF/IDLE (old-firmware heartbeat): not logged, it's every 10 s
        }
        if (phase_ == Phase::VISIT) { phase_ = Phase::IDLE; return note("visit reset before SHOW_ACTIVE"); }
        return Out();
    }

    Out startRun(uint32_t session, uint32_t now, bool replacing, const char* why) {
        if (!themeEverSet_ || themeSession_ != session) {           // R4: no guessing
            stats_.themeUnconfirmed++;
            if (phase_ == Phase::PLAYING) {
                phase_ = Phase::IDLE;
                return make(Act::TO_IDLE, runTheme_, "SHOW_ACTIVE but this show's theme was missed -> stale track stopped, sitting this show out", true);
            }
            phase_ = Phase::IDLE;
            return note("SHOW_ACTIVE but this show's theme was missed -> sitting this show out (no wrong theme)", true);
        }
        phase_ = Phase::PLAYING;
        runSession_ = session; runStart_ = now; runTheme_ = theme_;
        stats_.plays++;
        if (replacing) stats_.replaces++;
        return make(Act::PLAY, theme_, why);
    }

    static Out make(Act a, uint8_t th, const char* why, bool warn = false) {
        Out o; o.act = a; o.theme = th; o.why = why; o.warn = warn; return o;
    }
    Out note(const char* why, bool warn = false) const { return make(Act::NONE, theme_, why, warn); }

    Config   cfg_;
    Phase    phase_ = Phase::IDLE;
    uint8_t  theme_ = 0, runTheme_ = 0;
    bool     themeEverSet_ = false, everHeardLeader_ = false, leaderWasSilent_ = true, activeSeen_ = false;
    uint32_t themeSession_ = 0, visitSession_ = 0, runSession_ = 0;
    uint32_t visitStart_ = 0, runStart_ = 0, leaderRx_ = 0, lastPlayedMs_ = 0;
    uint32_t lastActiveSession_ = 0, lastActiveAt_ = 0;
    Stats    stats_;
};

}  // namespace showfollow
