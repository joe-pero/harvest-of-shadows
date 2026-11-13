#!/usr/bin/env python3
"""Projection test grid for fitting the round Stained Glass Window (Joe 2026-10-03).

Same idea as the Singing Pumpkins grid: project it, photograph the window from the
street, and read the circle's centre + radius off the grid in projector pixels.

Geometry (why it is drawn this way):
  * X605 native panel = 1024x768. The player outputs 1024x768 so one grid square = one
    panel pixel, and the circle can use the full 768 px short side (1080p input would be
    scaled to 1024x576 -> only 576 px across).
  * Projector lies on its side (AtmosFX vertical layout, same as the front door): the
    viewer sees a 768 wide x 1024 tall picture. Coordinates are in THAT upright space.
  * Rear projection mirrors the image. Projector MIRROR = NORMAL; the files carry the
    mirror (AtmosFX "Window (Reversed Text)" convention). So: draw upright as the street
    sees it -> flip left/right -> rotate 90 deg CW into the 1024x768 file (CCW until 2026-10-05).
  Self-check on the real window: "TOP" arrow at the top and the text reading normally
  from the street = rotation and mirror both right.

    python3 make_test_grid.py     # -> /mnt/media/Stain Glass 2026/Calibration/
"""
import os, subprocess
from PIL import Image, ImageDraw, ImageFont

VW, VH = 768, 1024          # upright, as seen from the street
MAJOR, MID, MINOR = 100, 50, 10
OUT = "/mnt/media/Stain Glass 2026/Calibration"
SECONDS = 600
CELL_COLOURS = [(0, 255, 255), (255, 0, 255), (255, 255, 0)]   # neighbours always differ
# Reference circles round the centre so a photo gives a first size estimate at a glance
REF_RADII = (150, 250, 350, 384)
# Measured window opening, file px (street view, AFTER the tilt fix) -> drawn thick green so the next photo shows the fit.
# 2026-10-05 IMG_9974 -> circle (372, 397) r370 (centre used for the tilt-fix rotation, keep it).
# 2026-10-05 IMG_9978 (tilt fixed): glass edge is an ELLIPSE in projector space (aimed up at an angle):
#   cv2.fitEllipse -> centre (372.2, 397.8), full axes 713.7 x 754.5, angle 20.3 ; rms 2.9 px (circle fit was 7.7).
FIT = (372, 397, 370)   # rotation centre (cx, cy, r) ; None = no tilt fix / no outline
FIT_ELLIPSE = ((372.2, 397.8), (713.7, 754.5), 20.3)   # cv2 RotatedRect ; None = draw the FIT circle instead
# The projector throws the picture rotated CW (IMG_9974: 7.7 / 7.0 deg). Counter-rotate the street view CCW
# about the window centre -> TOP points straight up. Round window = no loss. Show build must do the same.
TILT_FIX_DEG = 7.4      # PIL rotate(+) = counter-clockwise ; 0 = off


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
    big, small = font(17), font(16)
    for ci, x in enumerate(range(0, VW, MAJOR)):
        for ri, y in enumerate(range(0, VH, MAJOR)):
            d.rectangle([x + 3, y + 3, x + 39, y + 19], fill="black")
            d.text((x + 5, y + 3), f"{chr(65 + ci)}{ri + 1}", fill=CELL_COLOURS[(ci + ri) % 3], font=big)
    for x in range(MAJOR, VW, MAJOR):          # pixel numbers on all four edges
        for y, a in ((4, "mt"), (VH - 4, "mb")):
            d.text((x, y), str(x), fill=(255, 140, 0), font=small, anchor=a, stroke_width=3, stroke_fill="black")
    for y in range(MAJOR, VH, MAJOR):
        for x, a in ((6, "lm"), (VW - 6, "rm")):
            d.text((x, y), str(y), fill=(255, 140, 0), font=small, anchor=a, stroke_width=3, stroke_fill="black")
    cx, cy = VW // 2, VH // 2
    for r in REF_RADII:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(255, 0, 0), width=3)
        d.text((cx + 6, cy - r + 4), f"r{r}", fill=(255, 0, 0), font=small, stroke_width=3, stroke_fill="black")
    d.line([(cx - 40, cy), (cx + 40, cy)], fill=(0, 255, 0), width=5)
    d.line([(cx, cy - 40), (cx, cy + 40)], fill=(0, 255, 0), width=5)
    # orientation self-check
    d.polygon([(cx, 120), (cx - 45, 190), (cx + 45, 190)], fill=(0, 255, 0))
    d.text((cx, 200), "TOP", fill=(0, 255, 0), font=font(44), anchor="mt", stroke_width=4, stroke_fill="black")
    d.text((cx, VH - 150), "READS NORMALLY", fill=(0, 255, 0), font=font(38), anchor="mm", stroke_width=4, stroke_fill="black")
    d.text((cx, VH - 105), "FROM THE STREET", fill=(0, 255, 0), font=font(38), anchor="mm", stroke_width=4, stroke_fill="black")
    return im


def to_file(upright):
    """Street view -> file: mirror for rear projection, then 90 deg CW (projector on its side).
    Was 90 CCW until 2026-10-05: Joe's first real-window photo showed the picture rotated 180
    (projector lies on the other side from the AtmosFX _V convention), so the show build must
    rotate the AtmosFX _V files 180 too."""
    return upright.transpose(Image.FLIP_LEFT_RIGHT).transpose(Image.ROTATE_270)


def main():
    os.makedirs(OUT, exist_ok=True)
    up = grid()
    if TILT_FIX_DEG and FIT:
        up = up.rotate(TILT_FIX_DEG, resample=Image.BICUBIC, center=FIT[:2], fillcolor="black")
    d = ImageDraw.Draw(up)
    if FIT_ELLIPSE:
        import cv2
        (ex, ey), (ew, eh), ang = FIT_ELLIPSE
        poly = cv2.ellipse2Poly((round(ex), round(ey)), (round(ew / 2), round(eh / 2)), round(ang), 0, 360, 2)
        d.line([tuple(map(int, p)) for p in poly] + [tuple(map(int, poly[0]))], fill=(0, 255, 0), width=8)
        d.text((round(ex), round(ey + eh / 2) - 40), "FIT ELLIPSE", fill=(0, 255, 0), font=font(24), anchor="mm", stroke_width=4, stroke_fill="black")
    elif FIT:
        fx, fy, fr = FIT
        d.ellipse([fx - fr, fy - fr, fx + fr, fy + fr], outline=(0, 255, 0), width=8)
        d.text((fx, fy + fr - 30), f"FIT {fx},{fy} r{fr}", fill=(0, 255, 0), font=font(24), anchor="mm", stroke_width=4, stroke_fill="black")
    up.save(os.path.join(OUT, "Test grid (street view).png"))
    png = os.path.join(OUT, "Test grid.png")
    to_file(up).save(png)
    mp4 = os.path.join(OUT, "Test grid.mp4")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-loop", "1", "-framerate", "30", "-i", png,
                    "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-t", str(SECONDS),
                    "-c:v", "libx264", "-tune", "stillimage", "-preset", "veryfast", "-pix_fmt", "yuv420p",
                    "-profile:v", "high", "-level", "4.1", "-c:a", "aac", "-b:a", "64k", mp4], check=True)
    print("wrote", mp4)


if __name__ == "__main__":
    main()
