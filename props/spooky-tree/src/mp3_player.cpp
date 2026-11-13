#include "mp3_player.h"

static SafeYX5300 s_mp3(Serial2);

static uint32_t trackLengthMs(uint8_t track) {
    if (track == TRACK_AMBIENT) return LEN_AMBIENT_MS;
    if (track >= TRACK_ACTION_MIN && track <= TRACK_ACTION_MAX) return LEN_ACTION_MAX_MS;
    if (track >= TRACK_THEME_CORN && track <= TRACK_THEME_MPUMP) return LEN_THEME_MS;
    return 0;
}

void mp3Init() {
    SafeYX5300Config c;
    c.rxPin = MP3_RX_PIN;
    c.txPin = MP3_TX_PIN;
    c.loopCleanMs = MP3_LOOP_CLEAN_MS;   // P10: routine STOP+RESET+TF at an ambient wrap
    s_mp3.begin(c);
}

void     mp3Quiet()                    { s_mp3.quiet(MP3_RX_PIN, MP3_TX_PIN); }

void     mp3Tick()                     { s_mp3.tick(); }
bool     mp3PlayTrack(uint8_t track)   { return s_mp3.play(track, trackLengthMs(track)); }
bool     mp3PlayLoop(uint8_t track)    { return s_mp3.play(track, trackLengthMs(track), true); }   // length arms P9
void     mp3Stop(const char* why)      { s_mp3.stop(why); }
bool     mp3IsFinished()               { return s_mp3.finished(); }
void     mp3ClearFinished()            { s_mp3.clearFinished(); }
Mp3End   mp3EndReason()                { return s_mp3.endReason(); }
uint8_t  mp3CurrentTrack()             { return s_mp3.current(); }
uint32_t mp3GetLastActivityMs()        { return s_mp3.lastRxMs(); }
const SafeYX5300Stats& mp3Stats()      { return s_mp3.stats(); }
void     mp3Probe()                    { s_mp3.probe(); }
bool     mp3Echoing()                  { return s_mp3.echoing(); }
