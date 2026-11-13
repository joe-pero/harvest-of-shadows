#!/usr/bin/env python3
"""Front Door calibration grid (Joe 2026-10-05) -> /mnt/media/Front Door 2026/Calibration/.

Same idea as the Stain Glass grid (props/stain-glass-window/sdcard/make_test_grid.py):
project it on the real door, photograph from the street, read the door edges off the grid.

Geometry (matches the door clips, docs/front-door.md):
  * Player outputs 1920x1080; the projector (CP-DW10N) lies on its side, so the street sees a
    portrait 1080 wide x 1920 tall picture. Coordinates are in THAT upright space.
  * Front projection -> no mirror. Street view -> rotate 90 deg CCW into the file
    (same as the door clips: view upright = rotate CW).
  Self-check on the real door: "TOP" arrow at the top and the text reading normally from
  the street = rotation right (upside down = projector on its other side).
"""
import os, subprocess
from PIL import Image, ImageDraw, ImageFont

VW, VH = 1080, 1920         # upright, as seen from the street
MAJOR, MID, MINOR = 100, 50, 10
OUT = "/mnt/media/Front Door 2026/Calibration"
SECONDS = 600
# 2026-10-27: SUPERSEDED for fitting by the ArUco marker card (build/make_door_markers.py + read_door_markers.py);
# this grid is still imported by build/make_door_check.py / make_door_verify.py (font, to_file).
# Measured door opening (Joe street photos IMG_9970/9971, 2026-10-05; approx +/-20 px; void after the 10-27 projector move) ->
# drawn thick green so the next photo shows the fit. None = not drawn.
FIT = (505, 485, 1080, 1515)   # x0, y0, x1, y1
CELL_COLOURS = [(0, 255, 255), (255, 0, 255), (255, 255, 0)]   # neighbours always differ


def font(size):
    for f in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def grid():
    im = Image.new("RGB", (VW, VH), "black")
    d = ImageDraw.Draw(im)
    for step, col, w in ((MINOR, (90, 90, 90), 1), (MID, (170, 170, 170), 2), (MAJOR, (255, 255, 255), 3)):
        for x in range(0, VW + 1, step):
            d.line([(x, 0), (x, VH)], fill=col, width=w)
        for y in range(0, VH + 1, step):
            d.line([(0, y), (VW, y)], fill=col, width=w)
    big, small = font(20), font(18)
    for ci, x in enumerate(range(0, VW, MAJOR)):
        for ri, y in enumerate(range(0, VH, MAJOR)):
            d.rectangle([x + 3, y + 3, x + 46, y + 23], fill="black")
            d.text((x + 5, y + 3), f"{chr(65 + ci)}{ri + 1}", fill=CELL_COLOURS[(ci + ri) % 3], font=big)
    for x in range(MAJOR, VW, MAJOR):          # pixel numbers on all four edges
        for y, a in ((4, "mt"), (VH - 4, "mb")):
            d.text((x, y), str(x), fill=(255, 140, 0), font=small, anchor=a, stroke_width=3, stroke_fill="black")
    for y in range(MAJOR, VH, MAJOR):
        for x, a in ((6, "lm"), (VW - 6, "rm")):
            d.text((x, y), str(y), fill=(255, 140, 0), font=small, anchor=a, stroke_width=3, stroke_fill="black")
    d.rectangle([0, 0, VW - 1, VH - 1], outline=(255, 0, 0), width=6)   # picture edge
    if FIT:
        d.rectangle(FIT, outline=(0, 255, 0), width=10)
        d.text(((FIT[0] + FIT[2]) // 2, FIT[1] + 40), "FIT %d,%d - %d,%d" % FIT, fill=(0, 255, 0), font=font(30),
               anchor="mm", stroke_width=4, stroke_fill="black")
    cx, cy = VW // 2, VH // 2
    d.line([(cx - 50, cy), (cx + 50, cy)], fill=(0, 255, 0), width=6)
    d.line([(cx, cy - 50), (cx, cy + 50)], fill=(0, 255, 0), width=6)
    # orientation self-check
    d.polygon([(cx, 150), (cx - 60, 240), (cx + 60, 240)], fill=(0, 255, 0))
    d.text((cx, 255), "TOP", fill=(0, 255, 0), font=font(60), anchor="mt", stroke_width=5, stroke_fill="black")
    d.text((cx, VH - 230), "READS NORMALLY", fill=(0, 255, 0), font=font(56), anchor="mm", stroke_width=5, stroke_fill="black")
    d.text((cx, VH - 165), "FROM THE STREET", fill=(0, 255, 0), font=font(56), anchor="mm", stroke_width=5, stroke_fill="black")
    return im


def to_file(upright):
    """Street view -> file: 90 deg CCW (projector on its side), no mirror (front projection)."""
    return upright.transpose(Image.ROTATE_90)


def main():
    os.makedirs(OUT, exist_ok=True)
    up = grid()
    up.save(os.path.join(OUT, "Door grid (street view).png"))
    png = os.path.join(OUT, "Door grid.png")
    to_file(up).save(png)
    mp4 = os.path.join(OUT, "Door grid.mp4")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-loop", "1", "-framerate", "30", "-i", png,
                    "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-t", str(SECONDS),
                    "-c:v", "libx264", "-tune", "stillimage", "-preset", "veryfast", "-pix_fmt", "yuv420p",
                    "-profile:v", "high", "-level", "4.2", "-c:a", "aac", "-b:a", "64k", mp4], check=True)
    print("wrote", mp4)


if __name__ == "__main__":
    main()
