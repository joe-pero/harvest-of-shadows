#include "mp3_player.h"

static SafeYX5300 s_mp3(Serial2);

void mp3Init() {
    SafeYX5300Config c;
    c.rxPin = MP3_RX_PIN;
    c.txPin = MP3_TX_PIN;
    s_mp3.begin(c);
}

void     mp3Tick()                     { s_mp3.tick(); }
bool     mp3PlayTrack(uint8_t track)   { return s_mp3.play(track, track == TRACK_AMBIENT ? 0 : LEN_THEME_MS); }
bool     mp3PlayLoop(uint8_t track)    { return s_mp3.play(track, 0, true); }
void     mp3Stop(const char* why)      { s_mp3.stop(why); }
bool     mp3IsFinished()               { return s_mp3.finished(); }
void     mp3ClearFinished()            { s_mp3.clearFinished(); }
Mp3End   mp3EndReason()                { return s_mp3.endReason(); }
uint8_t  mp3CurrentTrack()             { return s_mp3.current(); }
uint32_t mp3GetLastActivityMs()        { return s_mp3.lastRxMs(); }
const SafeYX5300Stats& mp3Stats()      { return s_mp3.stats(); }
