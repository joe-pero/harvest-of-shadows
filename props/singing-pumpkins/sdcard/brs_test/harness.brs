' Test harness: real autorun.brs logic, BrightSign-only objects replaced by stand-ins.
Function MockTimer() as object
    return { id: Rnd(1000000), Stop: sub() : end sub, Start: sub() : end sub, SetElapsed: sub(a, b) : end sub, GetIdentity: function() : return m.id : end function }
End Function

Sub Main()
    s = {}
    s.errors = []
    s.cfg = LoadJson("pkg:/show.json")
    ApplyDefaults(s.cfg)
    s.serial = { SendLine: sub(l) : print "CARD> " + l : end sub }
    s.video = { PlayFile: function(f) : return true : end function, Stop: sub() : end sub, SetVolume: sub(v) : end sub }
    s.wd = MockTimer() : s.hb = MockTimer() : s.retry = MockTimer()
    LoadLibrary(s)
    s.mode = "DJ" : s.queue = [] : s.jukebox = [] : s.recent = [] : s.decks = {}
    s.cur = invalid : s.curFam = "" : s.runLen = 0 : s.playStart = 0 : s.lastShowEnd = -1000
    s.failStreak = 0 : s.played = { SUNO: 0.0, TSP: 0.0, ATMOS: 0.0 }
    PlayNext(s)
    for each cmd in ["STATUS", "SHOW", "SHOW", "END", "PLAY 5", "SKIP", "PLAY 62 NOW", "SHOW FORCE", "VOL 50", "BOGUS"]
        print "ESP32> " + cmd
        HandleLine(s, cmd)
    end for
    print "HARNESS DONE"
End Sub

Function UpTime(dummy as dynamic) as float
    g = GetGlobalAA()
    if g.fakeClock = invalid then g.fakeClock = 100.0
    g.fakeClock = g.fakeClock + 5.0
    return g.fakeClock
End Function
