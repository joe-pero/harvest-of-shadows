' Front Door -- CALIBRATION autorun (Joe 2026-10-05)
' Loops door_grid.mp4 at 1920x1080 (same mode as the door clips). Photograph the door from
' the street; read the door edges off the grid. The show files can stay on the card; put the
' show autorun.brs (props/front-door/sdcard/autorun.brs) back when done.
' OS 6.2 rule: no print (it goes out serial port 0).
Sub Main()
    mode = CreateObject("roVideoMode")
    mode.SetMode("1920x1080x60p")
    port = CreateObject("roMessagePort")
    v = CreateObject("roVideoPlayer")
    v.SetPort(port)
    v.SetLoopMode(true)
    ' 2026-10-05: was ok.ToStr() -> never wrote boot_ok.txt on the XT1143 (OS 6.2: no ToStr on Boolean)
    if v.PlayFile("door_grid.mp4") then
        WriteAsciiFile("SD:/boot_ok.txt", "door calibration autorun, PlayFile OK" + chr(10))
    else
        WriteAsciiFile("SD:/boot_ok.txt", "door calibration autorun, PlayFile FAILED" + chr(10))
    end if
    while true
        msg = wait(0, port)
    end while
End Sub
