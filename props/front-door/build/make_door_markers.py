#!/usr/bin/env python3
"""Front Door machine-readable calibration (Joe 2026-10-27: "you could see the data better than a human").

ArUco markers (DICT_4X4_1000) tile the whole street-view raster (1080x1920). Each id is one grid slot, so
every detected corner in a photo is an exact photo <-> projector-pixel pair. read_door_markers.py turns one
photo into the door corners. Output replaces `SD Card 2026 - calibration/door_grid.mp4` (calibration autorun
unchanged); the human grid/check clip stays buildable with make_door_check.py.

    python3 make_door_markers.py
"""
import os, subprocess, sys
import numpy as np, cv2

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "sdcard"))
import make_door_grid as g   # noqa: E402  (to_file = the door's street->file rotation)
from PIL import Image        # noqa: E402

VW, VH = 1080, 1920
MARKER, PITCH = 56, 72                     # px; 16 px white quiet zone between markers
COLS, ROWS = VW // PITCH, VH // PITCH      # 15 x 26 = 390 ids
X0, Y0 = (VW - COLS * PITCH) // 2 + (PITCH - MARKER) // 2, (VH - ROWS * PITCH) // 2 + (PITCH - MARKER) // 2
DICT = cv2.aruco.DICT_4X4_1000
CAL = "/mnt/media/Front Door 2026/SD Card 2026 - calibration"
SECONDS = 60                               # the calibration autorun loops it


def layout():
    """id -> 4 corners (TL, TR, BR, BL) in street-view projector px (same order ArUco reports)."""
    out = {}
    for r in range(ROWS):
        for c in range(COLS):
            x, y = X0 + c * PITCH, Y0 + r * PITCH
            out[r * COLS + c] = np.array([[x, y], [x + MARKER, y], [x + MARKER, y + MARKER], [x, y + MARKER]], float)
    return out


def image():
    d = cv2.aruco.getPredefinedDictionary(DICT)
    im = np.full((VH, VW), 255, np.uint8)
    for i, q in layout().items():
        x, y = q[0].astype(int)
        im[y:y + MARKER, x:x + MARKER] = cv2.aruco.generateImageMarker(d, i, MARKER)
    rgb = cv2.cvtColor(im, cv2.COLOR_GRAY2RGB)
    cv2.rectangle(rgb, (0, 0), (VW - 1, VH - 1), (255, 0, 0), 4)   # picture edge, for aiming (inside the quiet zone)
    return Image.fromarray(rgb)


def main():
    up = image()
    tmp = os.path.join(CAL, "door_markers.png")
    g.to_file(up).save(tmp)
    up.save(os.path.join("/mnt/media/Front Door 2026/Calibration", "door_markers (street view).png"))
    out = os.path.join(CAL, "door_grid.mp4")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-loop", "1", "-framerate", "30", "-i", tmp,
                    "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-t", str(SECONDS),
                    "-c:v", "libx264", "-tune", "stillimage", "-preset", "veryfast", "-crf", "12", "-pix_fmt", "yuv420p",
                    "-profile:v", "high", "-level", "4.2", "-c:a", "aac", "-b:a", "64k", out], check=True)
    os.remove(tmp)
    print("%d markers (%dx%d), wrote %s" % (COLS * ROWS, COLS, ROWS, out))


if __name__ == "__main__":
    main()
