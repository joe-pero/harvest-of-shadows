' =============================================================================
' Stain Glass Window - BrightSign 4K1142 (autorun.brs)
' =============================================================================
' Runs by itself at power-up. Plays window_loop.mp4 forever at the X605's native 1024x768.
' The Stain Glass ESP32 (link "wb", UART2 TX17/RX16) sends one line per command, 115200 8N1, CR:
'   ACTION <THEME>  glass_blowout -> one clip from that theme's list -> glass_restore -> loop
'   PLAY <file>     same sequence with any clip on the card (testing / picking clips)
'   END             cut straight back to the loop
'   VOL <0-100>     volume
'   STATUS / PING   report back
' Answers: READY, ACK, BUSY, ERR, HB, STATUS, PONG, NOW, DONE.
' All media is pre-fitted to the window (tilt fix, ellipse, mirror, 90 CW): props/stain-glass-window/build/.
' THEME -> clip lists below are a PROPOSAL until Joe confirms (docs/stain-glass-window.md).
' Same OS rules as the door + pumpkin cards: pure ASCII, no print, no .Count() on {} objects.
' =============================================================================

Sub Main()
    s = {}
    s.port = CreateObject("roMessagePort")
    s.loopFile = "window_loop.mp4"
    s.loopSecs = 400
    s.blowFile = "glass_blowout.mp4"
    s.restoreFile = "glass_restore.mp4"
    s.transSecs = 8
    s.actionSecs = 135          ' longest clip (Storm Crow 2:09) + margin; watchdog only
    s.slack = 10
    s.volume = 100
    ' PROPOSED mapping (Joe to confirm). A theme with several clips rotates through them.
    s.actions = {
        CORN: ["a_sc_startle1.mp4", "a_sc_startle2.mp4", "a_pk_thescarecrow.mp4"],
        ROTLD: ["a_grim_startle3.mp4", "a_wraith_faceofdeath.mp4", "a_pk_startle3.mp4"]
    }
    s.nextIdx = { CORN: 0, ROTLD: 0 }

    s.serial = CreateObject("roSerialPort", 0, 115200)
    diag = "window autorun.brs started" + chr(10)
    if s.serial <> invalid then
        s.serial.SetLineEventPort(s.port)
        diag = diag + "serial port 0 @ 115200: OPEN" + chr(10)
    else
        diag = diag + "serial port 0: FAILED TO OPEN" + chr(10)
    end if
    WriteAsciiFile("SD:/boot_ok.txt", diag)

    vm = CreateObject("roVideoMode")
    vm.SetMode("1024x768x60p")

    s.video = CreateObject("roVideoPlayer")
    s.video.SetPort(s.port)
    s.video.SetVolume(s.volume)

    s.wd = CreateObject("roTimer")
    s.wd.SetPort(s.port)
    s.hb = CreateObject("roTimer")
    s.hb.SetPort(s.port)
    s.retry = CreateObject("roTimer")
    s.retry.SetPort(s.port)

    s.mode = "LOOP"             ' LOOP | BLOW | ACTION | RESTORE
    s.cur = ""
    s.pending = ""
    s.playStart = 0
    s.failStreak = 0

    Send(s, "READY mode=LOOP")
    StartTimer(s.hb, 30)
    PlayLoop(s)

    while true
        msg = wait(0, s.port)
        t = type(msg)
        if t = "roVideoEvent" then
            if msg.GetInt() = 8 and (UpTime(0) - s.playStart) > 1.0 then MediaEnded(s)
        else if t = "roStreamLineEvent" then
            HandleLine(s, msg.GetString())
        else if t = "roTimerEvent" then
            id = msg.GetSourceIdentity()
            if id = s.wd.GetIdentity() then
                Send(s, "ERR watchdog " + s.cur + " did not end, back to loop")
                PlayLoop(s)
            else if id = s.hb.GetIdentity() then
                Send(s, "HB mode=" + s.mode + " now=" + s.cur)
                StartTimer(s.hb, 30)
            else if id = s.retry.GetIdentity() then
                PlayLoop(s)
            end if
        end if
    end while
End Sub

' Sequence: LOOP -> BLOW -> ACTION -> RESTORE -> LOOP. A failed step skips forward, never stalls.
Sub MediaEnded(s as object)
    if s.mode = "BLOW" then
        s.mode = "ACTION"
        if not PlayFileChecked(s, s.pending, s.actionSecs) then Restore(s)
    else if s.mode = "ACTION" then
        Send(s, "DONE " + s.cur)
        Restore(s)
    else
        PlayLoop(s)
    end if
End Sub

Sub Restore(s as object)
    s.mode = "RESTORE"
    if not PlayFileChecked(s, s.restoreFile, s.transSecs) then PlayLoop(s)
End Sub

Sub PlayLoop(s as object)
    s.mode = "LOOP"
    s.pending = ""
    if PlayFileChecked(s, s.loopFile, s.loopSecs) then
        Send(s, "NOW LOOP")
    else
        StartTimer(s.retry, 5)      ' bad card / missing loop: back off, never spin
    end if
End Sub

Function PlayFileChecked(s as object, f as string, secs as integer) as boolean
    s.wd.Stop()
    s.cur = f
    s.playStart = UpTime(0)
    if s.video.PlayFile(f) then
        s.failStreak = 0
        StartTimer(s.wd, secs + s.slack)
        return true
    end if
    s.failStreak = s.failStreak + 1
    Send(s, "ERR playfile " + f)
    return false
End Function

Sub StartSequence(s as object, clip as string, label as string)
    if s.mode <> "LOOP" then
        Send(s, "BUSY " + s.mode + " " + s.cur)
        return
    end if
    s.pending = clip
    s.mode = "BLOW"
    s.video.Stop()
    if PlayFileChecked(s, s.blowFile, s.transSecs) then
        Send(s, "ACK " + label + " " + clip)
    else
        ' no glass transition -> straight to the clip
        s.mode = "ACTION"
        if PlayFileChecked(s, clip, s.actionSecs) then
            Send(s, "ACK " + label + " " + clip + " (no blowout)")
        else
            PlayLoop(s)
        end if
    end if
End Sub

Sub HandleLine(s as object, line as string)
    line = line.Trim()
    if line = "" then return
    parts = line.Tokenize(" ")
    cmd = UCase(parts[0])
    if cmd = "ACTION" and parts.Count() >= 2 then
        theme = UCase(parts[1])
        lst = s.actions[theme]
        if lst = invalid then
            Send(s, "ERR no action " + parts[1])
        else
            i = s.nextIdx[theme]
            if i >= lst.Count() then i = 0
            s.nextIdx[theme] = i + 1
            StartSequence(s, lst[i], "ACTION " + theme)
        end if
    else if cmd = "PLAY" and parts.Count() >= 2 then
        StartSequence(s, parts[1], "PLAY")
    else if cmd = "END" then
        if s.mode <> "LOOP" then
            s.video.Stop()
            Send(s, "ACK END")
            PlayLoop(s)
        else
            Send(s, "ACK END (already loop)")
        end if
    else if cmd = "VOL" and parts.Count() >= 2 then
        s.volume = parts[1].ToInt()
        s.video.SetVolume(s.volume)
        Send(s, "ACK VOL " + Str(s.volume).Trim())
    else if cmd = "STATUS" then
        Send(s, "STATUS mode=" + s.mode + " now=" + s.cur + " vol=" + Str(s.volume).Trim() + " fails=" + Str(s.failStreak).Trim())
    else if cmd = "PING" then
        Send(s, "PONG")
    else
        Send(s, "ERR unknown command: " + line)
    end if
End Sub

Sub StartTimer(tm as object, secs as integer)
    tm.Stop()
    tm.SetElapsed(secs, 0)
    tm.Start()
End Sub

Sub Send(s as object, line as string)
    if s.serial <> invalid then s.serial.SendLine(line)
End Sub
