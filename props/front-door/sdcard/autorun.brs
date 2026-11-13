' =============================================================================
' Front Door projection - BrightSign XT1143 #2 (autorun.brs)
' =============================================================================
' Runs by itself at power-up. Plays Door_Loop.mp4 forever. The Stain Glass ESP32
' (link "db", UART1 TX22/RX21 -> HW-027 -> 3.5 mm) sends one line per command,
' 115200 8N1, ending in CR:
'   ACTION CORN     play Childern_action_door.mp4 once, then back to the loop
'   ACTION ROTLD    play ROTLD_action_door.mp4 once, then back to the loop
'   ACTION MUTANT   play Mutant_action_door.mp4 once, then back to the loop (Joe's video, 2026-10-06)
'   END             stop an action now, back to the loop
'   VOL <0-100>     volume
'   STATUS / PING   report back
' Answers: READY, ACK, BUSY, ERR, HB, STATUS, PONG, NOW, DONE.
' GPIO fallback (2026-10-06, serial 3.5 mm link not working): 12-pin GPIO block, Joe's 7-pin plug on pins 1-7.
'   GPIO 0 / 1 / 2 = inputs (1k pull-up in the player): ESP32 pulls one LOW briefly -> ACTION CORN / ROTLD / MUTANT.
'   GPIO 3 = status output: toggles every 1 s while the loop plays (= script alive), steady HIGH during an action.
'   Serial stays active too; whichever link works is used.
' Mutant Pumpkins theme: ACTION MUTANT (was ignored until Joe made the clip 2026-10-06).
' Same OS 6.2 rules as the Singing Pumpkins card (props/singing-pumpkins/sdcard/README.md):
' pure ASCII, no print (goes out serial port 0 too), no .Count() on {} objects.
' Docs: docs/front-door.md (harvest-of-shadows repo)
' =============================================================================

Sub Main()
    WriteAsciiFile("SD:/boot_ok.txt", "autorun.brs started, uptime " + Str(UpTime(0)) + chr(10))
    s = {}
    s.port = CreateObject("roMessagePort")
    s.loopFile = "Door_Loop.mp4"
    s.loopSecs = 702
    s.actions = { CORN: "Childern_action_door.mp4", ROTLD: "ROTLD_action_door.mp4", MUTANT: "Mutant_action_door.mp4" }
    s.actionSecs = 181
    s.slack = 10
    s.volume = 100
    s.gpioThemes = ["CORN", "ROTLD", "MUTANT"]   ' input pin n -> theme
    s.statusPin = 3
    s.statusOn = false
    s.lastGpio = -10

    s.serial = CreateObject("roSerialPort", 0, 115200)
    diag = "autorun.brs started" + chr(10)
    if s.serial <> invalid then
        s.serial.SetLineEventPort(s.port)
        diag = diag + "serial port 0 @ 115200: OPEN" + chr(10)
    else
        diag = diag + "serial port 0: FAILED TO OPEN" + chr(10)
    end if
    s.gpio = CreateObject("roControlPort", "BrightSign")
    if s.gpio <> invalid then
        s.gpio.SetPort(s.port)
        s.gpio.EnableInput(0)
        s.gpio.EnableInput(1)
        s.gpio.EnableInput(2)
        s.gpio.EnableOutput(s.statusPin)
        diag = diag + "gpio: 0-2 inputs, 3 status output" + chr(10)
    else
        diag = diag + "gpio: FAILED TO OPEN" + chr(10)
    end if
    WriteAsciiFile("SD:/boot_ok.txt", diag)

    vm = CreateObject("roVideoMode")
    vm.SetMode("1920x1080x60p")

    s.video = CreateObject("roVideoPlayer")
    s.video.SetPort(s.port)
    s.video.SetVolume(s.volume)

    s.wd = CreateObject("roTimer")
    s.wd.SetPort(s.port)
    s.hb = CreateObject("roTimer")
    s.hb.SetPort(s.port)
    s.retry = CreateObject("roTimer")
    s.retry.SetPort(s.port)
    s.blink = CreateObject("roTimer")
    s.blink.SetPort(s.port)

    s.mode = "LOOP"
    s.cur = ""
    s.playStart = 0
    s.failStreak = 0

    Send(s, "READY mode=LOOP")
    StartTimer(s.hb, 30)
    StartTimer(s.blink, 1)
    PlayLoop(s)

    while true
        msg = wait(0, s.port)
        t = type(msg)
        if t = "roVideoEvent" then
            ' 8 = media ended. Ignore a stale end event right after a new file started.
            if msg.GetInt() = 8 and (UpTime(0) - s.playStart) > 1.0 then MediaEnded(s)
        else if t = "roStreamLineEvent" then
            HandleLine(s, msg.GetString())
        else if t = "roControlDown" then
            pin = msg.GetInt()
            ' ignore bounce: one trigger per second at most
            if pin >= 0 and pin <= 2 and (UpTime(0) - s.lastGpio) > 1.0 then
                s.lastGpio = UpTime(0)
                StartAction(s, s.gpioThemes[pin], "GPIO " + Str(pin).Trim())
            end if
        else if t = "roTimerEvent" then
            id = msg.GetSourceIdentity()
            if id = s.wd.GetIdentity() then
                Send(s, "ERR watchdog " + s.cur + " did not end, restarting")
                MediaEnded(s)
            else if id = s.hb.GetIdentity() then
                Send(s, "HB mode=" + s.mode + " now=" + s.cur)
                StartTimer(s.hb, 30)
            else if id = s.retry.GetIdentity() then
                PlayLoop(s)
            else if id = s.blink.GetIdentity() then
                if s.mode = "ACTION" then
                    SetStatus(s, true)
                else
                    SetStatus(s, not s.statusOn)
                end if
                StartTimer(s.blink, 1)
            end if
        end if
    end while
End Sub

Sub MediaEnded(s as object)
    if s.mode = "ACTION" then Send(s, "DONE " + s.cur)
    PlayLoop(s)
End Sub

Sub SetStatus(s as object, onOff as boolean)
    s.statusOn = onOff
    if s.gpio <> invalid then s.gpio.SetOutputState(s.statusPin, onOff)
End Sub

' Serial ACTION and GPIO pins both end up here.
Sub StartAction(s as object, theme as string, via as string)
    f = s.actions[theme]
    if f = invalid then
        Send(s, "ERR no action " + theme)
    else if s.mode = "ACTION" then
        Send(s, "BUSY " + s.cur)
    else
        s.mode = "ACTION"
        SetStatus(s, true)
        s.video.Stop()
        if PlayFileChecked(s, f, s.actionSecs) then
            Send(s, "ACK ACTION " + theme + " via " + via)
        else
            s.mode = "LOOP"
        end if
    end if
End Sub

Sub PlayLoop(s as object)
    s.mode = "LOOP"
    if PlayFileChecked(s, s.loopFile, s.loopSecs) then Send(s, "NOW LOOP")
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
    ' Back off instead of spinning (bad card / missing file); retry the loop.
    StartTimer(s.retry, 5)
    return false
End Function

Sub HandleLine(s as object, line as string)
    line = line.Trim()
    if line = "" then return
    parts = line.Tokenize(" ")
    cmd = UCase(parts[0])
    if cmd = "ACTION" and parts.Count() >= 2 then
        StartAction(s, UCase(parts[1]), "serial")
    else if cmd = "END" then
        if s.mode = "ACTION" then
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
