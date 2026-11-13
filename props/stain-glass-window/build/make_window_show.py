#!/usr/bin/env python3
"""Stain Glass Window show media (Joe 2026-10-05) -> /mnt/media/Stain Glass 2026/SD Card 2026/.

Every AtmosFX _V clip -> 1024x768 file for the X605 on its side, rear projection:
  1. transpose=1        stored portrait (90 CCW) -> upright as authored
  2. hflip if the source is pre-mirrored (Window "Reversed Text" master + Joe's glass transitions) -> street view
  3. scale to COVER the window (width 764 = ellipse long axis 754 + margin; portrait, so top/bottom crop) and
     centre it on the tilt pivot (372, 397)
  4. rotate +7.4 deg CCW about the pivot (projector throws the picture 7.4 deg CW; IMG_9974)
  5. black outside the measured glass ellipse (+MARGIN px) (IMG_9978: centre (372.2,397.8), axes 713.7x754.5, 20.3 deg)
  6. street view -> file: hflip (rear projection) then transpose=1 (90 CW, IMG_9966 fix)
Geometry + numbers: docs/stain-glass-window.md. Grid that proved them: sdcard/make_test_grid.py.
"""
import os, subprocess, sys, math
import cv2, numpy as np

SRC = "/mnt/media/Stain Glass 2026/Main 2026"
OUT = "/mnt/media/Stain Glass 2026/SD Card 2026"
PIVOT = (372, 397)
TILT_FIX_DEG = 7.4
ELLIPSE = ((372.2, 397.8), (713.7, 754.5), 20.3)
MARGIN = 20                 # px past the measured edge (Joe: slightly larger, no dead area); spill hits the back of the facade
COVER_W = 810               # >= ellipse long axis 754 + 2*MARGIN + slack
CANVAS = 1600               # rotation canvas (> scaled clip height), pivot at its centre
VW, VH = 768, 1024          # street view

CLIPS = [  # (source, output name, pre-mirrored?)
    ("Window/Halloween Window_Win_V.mp4", "window_loop.mp4", True),
    ("Transitions/glass_blowout.mp4", "glass_blowout.mp4", True),
    ("Transitions/glass_restore.mp4", "glass_restore.mp4", True),
    ("Action/Grim Reaper_Startle Scare 3_Wall_V.mp4", "a_grim_startle3.mp4", False),
    ("Action/PHA_Wraith_FaceOfDeath_Holl_V.mp4", "a_wraith_faceofdeath.mp4", False),
    ("Action/Pumpkin King_Startle Scare3_Wall_V.mp4", "a_pk_startle3.mp4", False),
    ("Action/Pumpkin King_The Scarecrow_Wall_V.mp4", "a_pk_thescarecrow.mp4", False),
    ("Action/Scarecrow_Startle Scare1_Wall_V.mp4", "a_sc_startle1.mp4", False),
    ("Action/Scarecrow_Startle Scare2_Wall_V.mp4", "a_sc_startle2.mp4", False),
    ("Scarecrow/INF_Real_Raging Inferno_Wall_Cyclical_V.mp4", "s_raging_inferno.mp4", False),
    ("Scarecrow/PHA_Wraith_SoulSeeker_Holl_V.mp4", "s_wraith_soulseeker.mp4", False),
    ("Scarecrow/Pumpkin King_Hail to the King_Wall_V.mp4", "s_pk_hailtotheking.mp4", False),
    ("Scarecrow/Scarecrow_Fit To Be Tied_Wall_V.mp4", "s_sc_fittobetied.mp4", False),
    ("Scarecrow/Scarecrow_Storm Crow_Wall_V.mp4", "s_sc_stormcrow.mp4", False),
]


def make_mask(path):
    """RGBA street-view overlay: opaque black outside the (grown) ellipse, transparent inside."""
    (ex, ey), (ew, eh), ang = ELLIPSE
    inside = np.zeros((VH, VW), np.uint8)
    cv2.ellipse(inside, ((ex, ey), (ew + 2 * MARGIN, eh + 2 * MARGIN), ang), 255, -1, cv2.LINE_AA)
    rgba = np.zeros((VH, VW, 4), np.uint8)
    rgba[..., 3] = 255 - inside
    cv2.imwrite(path, rgba)


def build(src, out, mirrored, mask):
    ch = int(round(1920 * COVER_W / 1080))
    ox, oy = CANVAS // 2 - PIVOT[0], CANVAS // 2 - PIVOT[1]
    chain = ["transpose=1"] + (["hflip"] if mirrored else []) + [
        f"scale={COVER_W}:{ch}:flags=lanczos",
        f"pad={CANVAS}:{CANVAS}:{(CANVAS - COVER_W) // 2}:{(CANVAS - ch) // 2}:black",
        f"rotate={-math.radians(TILT_FIX_DEG):.6f}:fillcolor=black",   # ffmpeg: negative = counter-clockwise
        f"crop={VW}:{VH}:{ox}:{oy}"]
    fc = f"[0:v]{','.join(chain)}[v];[v][1:v]overlay=0:0:format=auto,hflip,transpose=1,format=yuv420p[o]"
    cmd = ["ffmpeg", "-v", "error", "-y", "-i", os.path.join(SRC, src), "-loop", "1", "-i", mask,
           "-filter_complex", fc, "-map", "[o]", "-map", "0:a?", "-shortest",
           "-c:v", "libx264", "-preset", "medium", "-crf", "19", "-profile:v", "high", "-level", "4.1",
           "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-movflags", "+faststart", os.path.join(OUT, out)]
    subprocess.run(cmd, check=True)


def main():
    os.makedirs(OUT, exist_ok=True)
    mask = os.path.join(OUT, "..", "Calibration", "window_mask.png")
    make_mask(mask)
    only = set(sys.argv[1:])
    for src, out, mir in CLIPS:
        if only and out not in only:
            continue
        build(src, out, mir, mask)
        print("built", out, flush=True)


if __name__ == "__main__":
    main()
