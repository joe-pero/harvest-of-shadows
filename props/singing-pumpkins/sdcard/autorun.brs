' =============================================================================
' Singing Pumpkins - BrightSign XT1143 show logic (autorun.brs)
' =============================================================================
' Runs by itself at power-up. No BrightAuthor presentation needed.
' Reads SD:/show.json (settings) + SD:/library.json (every video, made by
' build_card.py) and plays forever:
'
'   DJ mode   (default) : smart shuffle of Suno / TSP / Atmos songs + tales,
'                         weighted by PLAYING TIME per face set (show.json
'                         dj_share), short runs per face set, no recent repeats.
'                         Same face set = back to back, no fade. Face set
'                         changes = one iris transition file (old faces close
'                         into each pumpkin's centre, new faces open from it).
'   SHOW mode (on SHOW) : transition to Atmos -> 1 opening -> shuffled Clips 2.0
'                         -> 1 close-out, planned to fit show_seconds, then
'                         back to DJ.
'
' The ESP32 (ESP-NOW node) is the controller. It sends one line per command
' over the 3.5 mm serial jack, 115200 8N1, ending in CR:
'   SHOW            start the trick-or-treat show (BUSY if running / cooldown)
'   SHOW FORCE      manual override: start a fresh show now, no matter what
'   END             finish the show early (per show.json end_mode)
'   PLAY <n>        jukebox: play #n next
'   PLAY <n> NOW    manual override: play #n right now (ends a running show)
'   SKIP            next clip now          VOL <0-100>   volume
'   LOOP <n>        play clip n over and over (projection grid); LOOP 0 = back to the DJ loop
'   LIST            send the pick list: SONG <n> <label> ... then LISTEND <count>
'   STATUS / PING   report back
' The card answers with lines: READY, NOW, CUES, MODE, ACK, BUSY, ERR, HB, STATUS, PONG,
' SONG, LISTEND.
' Docs: docs/singing-pumpkins.md  (harvest-of-shadows repo)
' =============================================================================

Sub Main()
    ' Boot marker: present = the script compiled and started (no file = it never ran).
    WriteAsciiFile("SD:/boot_ok.txt", "autorun.brs started, uptime " + Str(UpTime(0)) + chr(10))
    s = {}
    s.port = CreateObject("roMessagePort")
    s.errors = []

    s.cfg = LoadJson("SD:/show.json")
    if s.cfg = invalid then
        s.errors.Push("show.json missing or bad JSON, using defaults")
        s.cfg = {}
    end if
    ApplyDefaults(s.cfg)

    serial = CreateObject("roSerialPort", s.cfg.serial_port, s.cfg.serial_baud)
    s.serial = serial
    diag = "autorun.brs started" + chr(10)
    if serial <> invalid then
        serial.SetLineEventPort(s.port)
        diag = diag + "serial port " + Str(s.cfg.serial_port).Trim() + " @ " + Str(s.cfg.serial_baud).Trim() + ": OPEN" + chr(10)
    else
        diag = diag + "serial port " + Str(s.cfg.serial_port).Trim() + ": FAILED TO OPEN (invalid)" + chr(10)
    end if
    ' Which other port numbers exist on this player/OS (serial wiring check, 2026-09-27).
    for p = 0 to 3
        if p <> s.cfg.serial_port then
            probe = CreateObject("roSerialPort", p, s.cfg.serial_baud)
            if probe <> invalid then diag = diag + "  port " + Str(p).Trim() + " exists" + chr(10) else diag = diag + "  port " + Str(p).Trim() + " none" + chr(10)
            probe = invalid
        end if
    end for
    WriteAsciiFile("SD:/boot_ok.txt", diag)
    ' First ~60 s: heartbeat every 0.5 s so a meter / the ESP32 cannot miss it.
    s.hbBurst = 120

    vm = CreateObject("roVideoMode")
    if s.cfg.video_mode <> "" then vm.SetMode(s.cfg.video_mode)

    s.video = CreateObject("roVideoPlayer")
    s.video.SetPort(s.port)
    s.video.SetVolume(s.cfg.volume)

    s.wd = CreateObject("roTimer")
    s.wd.SetPort(s.port)
    s.hb = CreateObject("roTimer")
    s.hb.SetPort(s.port)
    s.retry = CreateObject("roTimer")
    s.retry.SetPort(s.port)

    SeedRandom()
    LoadLibrary(s)

    s.mode = "DJ"
    s.queue = []
    s.jukebox = []
    s.recent = []
    s.decks = {}
    s.cur = invalid
    s.curFam = ""
    s.runLen = 0
    s.playStart = 0
    s.lastShowEnd = -1000
    s.failStreak = 0
    s.played = { SUNO: 0.0, TSP: 0.0, ATMOS: 0.0 }

    Send(s, "READY items=" + Txt(s.items.Count()) + " mode=DJ")
    for each e in s.errors
        Send(s, "ERR " + e)
    end for
    StartHeartbeat(s)
    PlayNext(s)

    while true
        msg = wait(0, s.port)
        t = type(msg)
        if t = "roVideoEvent" then
            ' 8 = media ended. Ignore a stale end event right after we started a new file.
            if msg.GetInt() = 8 and (UpTime(0) - s.playStart) > 1.0 then PlayNext(s)
        else if t = "roStreamLineEvent" then
            HandleLine(s, msg.GetString())
        else if t = "roTimerEvent" then
            id = msg.GetSourceIdentity()
            if id = s.wd.GetIdentity() then
                if s.cur <> invalid then Send(s, "ERR watchdog " + s.cur.id + " did not end, skipping")
                PlayNext(s)
            else if id = s.hb.GetIdentity() then
                now = "none"
                if s.cur <> invalid then now = Txt(s.cur.n) + " " + s.cur.id
                Send(s, "HB mode=" + s.mode + " now=" + now)
                StartHeartbeat(s)
            else if id = s.retry.GetIdentity() then
                PlayNext(s)
            end if
        end if
    end while
End Sub

' ---------------------------------------------------------------- settings ---
Sub ApplyDefaults(c as object)
    d = {}
    d.volume = 100
    d.video_mode = ""
    d.serial_port = 0
    d.serial_baud = 115200
    d.max_run = 2
    d.no_repeat_window = 25
    d.show_seconds = 180
    d.end_mode = "after_clip"
    d.show_cooldown_seconds = 20
    d.watchdog_slack_seconds = 8
    d.heartbeat_seconds = 30
    d.loop_n = 0
    for each k in d
        if c[k] = invalid then c[k] = d[k]
    end for
    if c.dj_share = invalid then c.dj_share = { SUNO: 40, TSP: 30, ATMOS: 30 }
    if c.atmos_mix = invalid then c.atmos_mix = { atmos_song: 60, atmos_tale: 35, atmos_scare: 5 }
End Sub

Function LoadJson(path as string) as dynamic
    body = ReadAsciiFile(path)
    if body = invalid or body = "" then return invalid
    return ParseJson(body)
End Function

Sub LoadLibrary(s as object)
    s.items = []
    s.pools = {}
    s.transitions = {}
    s.byN = {}
    lib = LoadJson("SD:/library.json")
    if lib = invalid or lib.items = invalid then
        s.errors.Push("library.json missing or bad JSON, nothing to play")
        return
    end if
    for each it in lib.items
        s.items.Push(it)
        cat = it.category
        if cat = "transition" then
            s.transitions[it["from"] + ">" + it.family] = it
        else if cat = "buffer" then
            ' on the card, not used by the logic
        else
            if s.pools[cat] = invalid then s.pools[cat] = []
            s.pools[cat].Push(it)
            if it.n <> invalid and it.n > 0 then s.byN[Txt(it.n)] = it
        end if
    end for
End Sub

' ---------------------------------------------------------------- playback ---
Sub PlayNext(s as object)
    s.wd.Stop()
    s.retry.Stop()
    if s.queue.Count() = 0 then
        if s.mode = "SHOW" then
            s.mode = "DJ"
            s.lastShowEnd = UpTime(0)
            Send(s, "MODE DJ")
        end if
        ' loop_n (show.json, or LOOP n from the ESP32): play that one clip over and over
        ' instead of the DJ loop. Projection test grid, Joe 2026-10-02.
        lp = invalid
        if s.cfg.loop_n <> invalid and s.cfg.loop_n > 0 then lp = s.byN[Txt(s.cfg.loop_n)]
        if lp <> invalid then
            s.queue.Push(lp)
        else
            RefillDJ(s)
        end if
    end if
    it = s.queue.Shift()
    if it = invalid then
        Send(s, "ERR nothing to play")
        StartRetry(s)
        return
    end if
    ok = s.video.PlayFile("SD:/" + it.file)
    s.playStart = UpTime(0)
    if not ok then
        s.failStreak = s.failStreak + 1
        Send(s, "ERR playfile " + it.file)
        if s.failStreak >= 10 then
            s.failStreak = 0
            StartRetry(s)
        else
            PlayNext(s)
        end if
        return
    end if
    s.failStreak = 0
    s.cur = it
    if it.family <> s.curFam then
        s.curFam = it.family
        s.runLen = 0
    end if
    if it.category <> "transition" then
        s.runLen = s.runLen + 1
        Remember(s, it.id)
        if s.mode = "DJ" and s.played[it.family] <> invalid then s.played[it.family] = s.played[it.family] + it.dur   ' TEST clips have no share
    end if
    s.wd.SetElapsed(Int(it.dur) + s.cfg.watchdog_slack_seconds, 0)
    s.wd.Start()
    Send(s, "NOW " + Txt(it.n) + " " + it.id + " " + it.family + " " + it.category + " " + Txt(it.dur))
    ' Skeleton jaw moments for this clip (library.json "skel", skel_cues.py). The ESP32 decides
    ' whether to use one and times it from the NOW line; the card only passes them on.
    if it.skel <> invalid then Send(s, "CUES " + Txt(it.n) + " " + it.skel)
End Sub

Sub StartRetry(s as object)
    s.retry.SetElapsed(5, 0)
    s.retry.Start()
End Sub

Sub StartHeartbeat(s as object)
    if s.hbBurst <> invalid and s.hbBurst > 0 then
        s.hbBurst = s.hbBurst - 1
        s.hb.SetElapsed(0, 500)
        s.hb.Start()
        return
    end if
    s.hb.SetElapsed(s.cfg.heartbeat_seconds, 0)
    s.hb.Start()
End Sub

' Queue the transition from the face set on screen to `fam` (nothing if same set).
Sub QueueBridges(s as object, fam as string)
    if fam = s.curFam or s.curFam = "" then return
    tr = s.transitions[s.curFam + ">" + fam]
    if tr = invalid then
        ' TEST (projection grid) has no transitions on purpose: hard cut, no error
        if fam <> "TEST" and s.curFam <> "TEST" then Send(s, "ERR no transition " + s.curFam + ">" + fam + ", hard cut")
    else
        s.queue.Push(tr)
    end if
End Sub

' ---------------------------------------------------------------- DJ mode ---
Sub RefillDJ(s as object)
    ' Jukebox picks win over the shuffle.
    if s.jukebox.Count() > 0 then
        it = s.jukebox.Shift()
        QueueBridges(s, it.family)
        s.queue.Push(it)
        return
    end if
    for attempt = 1 to 3
        fam = PickFamily(s)
        it = invalid
        if fam = "SUNO" then
            it = Draw(s, "suno")
        else if fam = "TSP" then
            it = Draw(s, "tsp")
        else if fam = "ATMOS" then
            it = Draw(s, PickWeighted(s.cfg.atmos_mix, s.pools))
        end if
        if it <> invalid then
            QueueBridges(s, it.family)
            s.queue.Push(it)
            return
        end if
    end for
End Sub

' Face set whose share of recent playing time is furthest below its target,
' with a little randomness and a bonus to stay put (up to max_run in a row).
Function PickFamily(s as object) as string
    total = s.played.SUNO + s.played.TSP + s.played.ATMOS
    best = ""
    bestScore = -1000.0
    for each fam in ["SUNO", "TSP", "ATMOS"]
        share = s.cfg.dj_share[fam]
        if share <> invalid and share > 0 and FamilyHasContent(s, fam) then
            if not (fam = s.curFam and s.runLen >= s.cfg.max_run) then
                pct = 0.0
                if total > 0 then pct = s.played[fam] * 100.0 / total
                score = share - pct + Rnd(0) * 15.0
                if fam = s.curFam then score = score + 8.0
                if score > bestScore then
                    bestScore = score
                    best = fam
                end if
            end if
        end if
    end for
    ' Let old history fade so the mix tracks the last few hours, not the whole night.
    for each fam in ["SUNO", "TSP", "ATMOS"]
        s.played[fam] = s.played[fam] * 0.97
    end for
    if best = "" then best = s.curFam
    return best
End Function

Function FamilyHasContent(s as object, fam as string) as boolean
    if fam = "SUNO" then return s.pools.suno <> invalid
    if fam = "TSP" then return s.pools.tsp <> invalid
    return s.pools.atmos_song <> invalid or s.pools.atmos_tale <> invalid
End Function

' Weighted random key from a {key: weight} map, only keys that have a pool.
Function PickWeighted(w as object, pools as object) as string
    total = 0
    for each k in w
        if pools[k] <> invalid then total = total + w[k]
    end for
    if total <= 0 then return "atmos_song"
    r = Rnd(0) * total
    for each k in w
        if pools[k] <> invalid then
            r = r - w[k]
            if r < 0 then return k
        end if
    end for
    return "atmos_song"
End Function

' Shuffle-deck draw (Fisher-Yates), skipping anything in the recent window.
Function Draw(s as object, cat as string) as dynamic
    pool = s.pools[cat]
    if pool = invalid or pool.Count() = 0 then return invalid
    win = s.cfg.no_repeat_window
    if win > pool.Count() - 1 then win = pool.Count() - 1
    tries = 0
    while tries < pool.Count() * 2
        deck = s.decks[cat]
        if deck = invalid or deck.Count() = 0 then
            deck = Shuffled(pool)
            s.decks[cat] = deck
        end if
        it = deck.Pop()
        if not IsRecent(s, it.id, win) then return it
        tries = tries + 1
    end while
    return pool[Rnd(pool.Count()) - 1]
End Function

Function Shuffled(pool as object) as object
    a = []
    for each x in pool
        a.Push(x)
    end for
    i = a.Count() - 1
    while i > 0
        j = Rnd(i + 1) - 1
        tmp = a[i]
        a[i] = a[j]
        a[j] = tmp
        i = i - 1
    end while
    return a
End Function

Sub Remember(s as object, id as string)
    s.recent.Push(id)
    while s.recent.Count() > 200
        s.recent.Shift()
    end while
End Sub

Function IsRecent(s as object, id as string, win as integer) as boolean
    n = s.recent.Count()
    i = n - 1
    while i >= 0 and i >= n - win
        if s.recent[i] = id then return true
        i = i - 1
    end while
    return false
End Function

' ---------------------------------------------------------------- SHOW mode ---
Sub StartShow(s as object, force as boolean)
    if not force then
        if s.mode = "SHOW" then
            Send(s, "BUSY show already running")
            return
        end if
        if UpTime(0) - s.lastShowEnd < s.cfg.show_cooldown_seconds then
            Send(s, "BUSY cooldown")
            return
        end if
    end if
    if force then Trace(s, "1 start FORCE") else Trace(s, "1 start")
    opening = Draw(s, "tot_open")
    closing = Draw(s, "tot_close")
    Trace(s, "2 drew open/close")
    if opening = invalid or closing = invalid or s.pools.tot_clip = invalid then
        Send(s, "ERR show content missing (tot_open / tot_clip / tot_close)")
        return
    end if

    ' Interrupt whatever DJ was playing; the show starts now.
    s.mode = "SHOW"
    s.queue = []
    s.video.Stop()
    Trace(s, "3 video stopped")
    QueueBridges(s, "ATMOS")
    s.queue.Push(opening)

    ' Fill the middle with shuffled clips that fit the time left.
    remain = s.cfg.show_seconds - opening.dur - closing.dur
    used = {}
    nClips = 0     ' not used.Count(): an associative array has no Count() on OS 6.2 (killed SHOW, 2026-09-27)
    pool = s.pools.tot_clip
    tries = 0
    while remain > 10 and tries < pool.Count() * 2
        c = Draw(s, "tot_clip")
        tries = tries + 1
        if c <> invalid and used[c.id] = invalid and c.dur <= remain then
            s.queue.Push(c)
            used[c.id] = true
            nClips = nClips + 1
            remain = remain - c.dur
        end if
    end while
    s.queue.Push(closing)
    Trace(s, "4 clips planned")
    planned = s.cfg.show_seconds - remain
    Send(s, "MODE SHOW clips=" + Txt(nClips) + " planned=" + Txt(Int(planned)) + "s")
    ' Keep the DJ from continuing an old run after the show.
    s.runLen = s.cfg.max_run
    PlayNext(s)
    Trace(s, "5 playing")
End Sub

' Show-start breadcrumbs on the card (2026-09-27: SHOW made the XT1143 go silent + reboot, the same
' logic runs clean in a BrightScript interpreter). Last line in SD:/show_trace.txt = last step reached.
Sub Trace(s as object, what as string)
    if s.trace = invalid then s.trace = ""
    s.trace = s.trace + "uptime " + Txt(Int(UpTime(0))) + " show " + what + chr(10)
    WriteAsciiFile("SD:/show_trace.txt", s.trace)
End Sub

Sub EndShow(s as object)
    if s.mode <> "SHOW" then
        Send(s, "ACK END (no show running)")
        return
    end if
    ' Drop the remaining middle clips, keep the close-out.
    keep = []
    for each q in s.queue
        if q.category = "tot_close" then keep.Push(q)
    end for
    s.queue = keep
    Send(s, "ACK END " + s.cfg.end_mode)
    if s.cfg.end_mode = "now" and s.cur <> invalid and s.cur.category <> "tot_close" then
        s.video.Stop()
        PlayNext(s)
    end if
End Sub

' ---------------------------------------------------------------- serial ---
Sub HandleLine(s as object, line as string)
    line = line.Trim()
    if line = "" then return
    parts = line.Tokenize(" ")
    cmd = UCase(parts[0])
    if cmd = "SHOW" then
        StartShow(s, parts.Count() >= 2 and UCase(parts[1]) = "FORCE")
    else if cmd = "END" then
        EndShow(s)
    else if cmd = "PLAY" and parts.Count() >= 2 then
        it = s.byN[parts[1]]
        if it = invalid then
            Send(s, "ERR no song #" + parts[1])
        else if parts.Count() >= 3 and UCase(parts[2]) = "NOW" then
            ' Manual pick wins: a running show is ended, no close-out.
            if s.mode = "SHOW" then
                s.mode = "DJ"
                s.lastShowEnd = UpTime(0)
                Send(s, "MODE DJ (manual pick)")
            end if
            s.queue = []
            QueueBridges(s, it.family)
            s.queue.Push(it)
            s.video.Stop()
            Send(s, "ACK PLAY " + Txt(it.n) + " NOW")
            PlayNext(s)
        else
            s.jukebox.Push(it)
            Send(s, "ACK PLAY " + Txt(it.n) + " NEXT")
        end if
    else if cmd = "LOOP" and parts.Count() >= 2 then
        ' LOOP <n>: play clip n over and over (until reboot / LOOP 0). LOOP 0: back to the DJ loop now.
        n = parts[1].ToInt()
        if n > 0 and s.byN[Txt(n)] = invalid then
            Send(s, "ERR no song #" + parts[1])
        else
            s.cfg.loop_n = n
            Send(s, "ACK LOOP " + Txt(n))
            s.queue = []
            s.video.Stop()
            PlayNext(s)
        end if
    else if cmd = "SKIP" then
        s.video.Stop()
        PlayNext(s)
    else if cmd = "VOL" and parts.Count() >= 2 then
        s.cfg.volume = parts[1].ToInt()
        s.video.SetVolume(s.cfg.volume)
        Send(s, "ACK VOL " + Txt(s.cfg.volume))
    else if cmd = "STATUS" then
        now = "none"
        if s.cur <> invalid then now = Txt(s.cur.n) + " " + s.cur.id + " " + s.cur.family
        Send(s, "STATUS mode=" + s.mode + " now=" + now + " queued=" + Txt(s.queue.Count()) + " jukebox=" + Txt(s.jukebox.Count()))
    else if cmd = "LIST" then
        count = 0
        for each it in s.items
            if it.label <> invalid and it.n <> invalid and it.n > 0 then
                Send(s, "SONG " + Txt(it.n) + " " + it.label)
                count = count + 1
            end if
        end for
        Send(s, "LISTEND " + Txt(count))
    else if cmd = "PING" then
        Send(s, "PONG")
    else
        Send(s, "ERR unknown command: " + line)
    end if
End Sub

Sub Send(s as object, line as string)
    ' No "print": on the XT1143 (OS 6.2) print goes out of serial port 0 too (debug console),
    ' so every line reached the ESP32 twice and the pick list came in at 306 of 153 (2026-09-27).
    if s.serial <> invalid then
        s.serial.SendLine(line)
    else
        print line
    end if
End Sub

' ---------------------------------------------------------------- helpers ---
Function Txt(x as dynamic) as string
    if x = invalid then return "-"
    return Str(x).Trim()
End Function

Sub SeedRandom()
    ' Rnd() may start from the same seed every boot: burn a clock-dependent number of draws.
    t = CreateObject("roSystemTime").GetLocalDateTime()
    n = (t.GetMillisecond() + t.GetSecond() * 17 + t.GetMinute() * 31) mod 997
    for i = 0 to n
        x = Rnd(0)
    end for
End Sub
