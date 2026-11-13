' Stained Glass Window -- CALIBRATION card (Joe 2026-10-03)
' Loops test_grid.mp4 at the X605's native 1024x768 so one grid square = one panel pixel.
' Photograph the window from the street; read the circle centre + radius off the grid.
' Replaced by the show autorun once the circle is measured.
Sub Main()
    mode = CreateObject("roVideoMode")
    mode.SetMode("1024x768x60p")
    port = CreateObject("roMessagePort")
    v = CreateObject("roVideoPlayer")
    v.SetPort(port)
    v.SetLoopMode(true)
    if not v.PlayFile("test_grid.mp4") then print "test_grid.mp4 failed to play"
    while true
        msg = wait(0, port)
    end while
End Sub
