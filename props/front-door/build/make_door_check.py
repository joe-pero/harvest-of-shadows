#!/usr/bin/env python3
"""Front Door re-fit check clip (Joe 2026-10-27: "aspect issue", redo the grid + photo loop).

One calibration video that alternates:
  1. GRID  (15 s)  make_door_grid.py's grid with the CURRENT door shape drawn on it (green outline,
                   read from door_mask.png = exactly what make_door_fitted.sh uses) + its corner numbers.
  2. CHECK (20 s)  a test pattern pushed through make_door_fitted.sh = the same crop + warp + mask as the
                   show clips. From the street: circles should look round, squares square, the white
                   border should sit on the door edges. Edge ticks (1-9 = tenths) name a spot to nudge.
Output replaces `SD Card 2026 - calibration/door_grid.mp4` (old one kept as door_grid_2026-10-05.mp4),
so the calibration autorun.brs is unchanged.

    python3 make_door_check.py            # current corners (BL_WIDEN etc. as make_door_fitted.sh)
    BL_WIDEN=20 python3 make_door_check.py
"""
import os, shutil, subprocess, sys, tempfile
import numpy as np, cv2
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "sdcard"))
import make_door_grid as g   # noqa: E402

CAL = "/mnt/media/Front Door 2026/SD Card 2026 - calibration"
MASK = "/mnt/media/Front Door 2026/Calibration/door_mask.png"
VW, VH = g.VW, g.VH
GRID_S, CHECK_S, REPEATS = 15, 20, 17      # 35 s cycle, ~10 min file like the old grid


def check_pattern():
    """Upright 1080x1920 content: what a clip looks like before the door warp."""
    im = Image.new("RGB", (VW, VH), "black")
    d = ImageDraw.Draw(im)
    sq = 120
    for y in range(0, VH, sq):                 # dim checkerboard = square test
        for x in range(0, VW, sq):
            if (x // sq + y // sq) % 2:
                d.rectangle([x, y, x + sq - 1, y + sq - 1], fill=(55, 55, 70))
    for (cx, cy), r in (((540, 960), 450), ((540, 340), 180), ((540, 1580), 180)):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(255, 255, 255), width=10)
    d.line([(440, 960), (640, 960)], fill=(0, 255, 0), width=8)
    d.line([(540, 860), (540, 1060)], fill=(0, 255, 0), width=8)
    d.polygon([(540, 600), (490, 680), (590, 680)], fill=(0, 255, 0))
    d.text((540, 690), "TOP", fill=(0, 255, 0), font=g.font(60), anchor="mt", stroke_width=5, stroke_fill="black")
    for i in range(1, 10):                     # tenths along every edge
        x, y = VW * i // 10, VH * i // 10
        for p, a in (((x, 70), "mm"), ((x, VH - 70), "mm"), ((70, y), "mm"), ((VW - 70, y), "mm")):
            d.text(p, str(i), fill=(255, 220, 0), font=g.font(44), anchor=a, stroke_width=4, stroke_fill="black")
        d.line([(x, 0), (x, 30)], fill=(255, 220, 0), width=6)
        d.line([(x, VH - 30), (x, VH)], fill=(255, 220, 0), width=6)
        d.line([(0, y), (30, y)], fill=(255, 220, 0), width=6)
        d.line([(VW - 30, y), (VW, y)], fill=(255, 220, 0), width=6)
    d.rectangle([0, 0, VW - 1, VH - 1], outline=(255, 255, 255), width=14)   # clip edge
    return im


def door_quad():
    """The door shape make_door_fitted.sh just wrote (transparent area of the mask), 4 corners."""
    a = cv2.imread(MASK, cv2.IMREAD_UNCHANGED)[..., 3]
    cs, _ = cv2.findContours((a < 128).astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    c = max(cs, key=cv2.contourArea)
    q = cv2.approxPolyDP(c, 4, True).reshape(-1, 2)
    if len(q) != 4:
        sys.exit("door mask is not a 4-corner shape: %s" % q.tolist())
    s, df = q.sum(1), q[:, 0] - q[:, 1]        # order TL, TR, BR, BL
    return [tuple(map(int, q[i])) for i in (s.argmin(), df.argmax(), s.argmax(), df.argmin())]


def still_mp4(png, secs, out):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-loop", "1", "-framerate", "30", "-i", png,
                    "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-t", str(secs),
                    "-c:v", "libx264", "-tune", "stillimage", "-preset", "veryfast", "-pix_fmt", "yuv420p",
                    "-profile:v", "high", "-level", "4.2", "-c:a", "aac", "-b:a", "64k", out], check=True)


def main():
    tmp = tempfile.mkdtemp(prefix="door_check_")
    src, fit = os.path.join(tmp, "in"), os.path.join(tmp, "out")
    os.makedirs(src)
    g.to_file(check_pattern()).save(os.path.join(tmp, "check.png"))
    still_mp4(os.path.join(tmp, "check.png"), CHECK_S, os.path.join(src, "check.mp4"))
    subprocess.run([os.path.join(HERE, "make_door_fitted.sh"), "check.mp4"], check=True,
                   env=dict(os.environ, IN=src, OUT=fit))

    g.FIT = None                                # grid without the old 10-05 rectangle
    up = g.grid()
    tl, tr, br, bl = q = door_quad()
    d = ImageDraw.Draw(up)
    d.line(q + [q[0]], fill=(0, 255, 0), width=8)
    f = g.font(26)
    for (x, y), a in ((tl, "lt"), (tr, "rt"), (br, "rb"), (bl, "lb")):
        d.text((x + (12 if a[0] == "l" else -12), y + (14 if a[1] == "t" else -14)), "%d,%d" % (x, y),
               fill=(0, 255, 0), font=f, anchor=a, stroke_width=4, stroke_fill="black")
    up.save(os.path.join(tmp, "grid_up.png"))
    g.to_file(up).save(os.path.join(tmp, "grid.png"))
    still_mp4(os.path.join(tmp, "grid.png"), GRID_S, os.path.join(tmp, "grid.mp4"))

    lst = os.path.join(tmp, "list.txt")
    with open(lst, "w") as fh:
        fh.write(("file '%s'\nfile '%s'\n" % (os.path.join(tmp, "grid.mp4"), os.path.join(fit, "check.mp4"))) * REPEATS)
    out = os.path.join(tmp, "door_grid.mp4")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", lst,
                    "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-pix_fmt", "yuv420p",
                    "-profile:v", "high", "-level", "4.2", "-r", "30",
                    "-c:a", "aac", "-b:a", "64k", "-ar", "48000", "-movflags", "+faststart", out], check=True)

    old = os.path.join(CAL, "door_grid_2026-10-05.mp4")
    if not os.path.exists(old):
        shutil.copy2(os.path.join(CAL, "door_grid.mp4"), old)
    shutil.copy2(out, os.path.join(CAL, "door_grid.mp4"))
    for n in ("grid_up.png", "check.png"):
        shutil.copy2(os.path.join(tmp, n), os.path.join("/mnt/media/Front Door 2026/Calibration", "check_" + n))
    print("door corners TL %s TR %s BR %s BL %s" % (tl, tr, br, bl))
    print("wrote", os.path.join(CAL, "door_grid.mp4"), "(tmp kept:", tmp + ")")


if __name__ == "__main__":
    main()
