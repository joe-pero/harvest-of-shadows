#!/usr/bin/env python3
"""Projection test grid for fitting the pumpkin faces (Joe 2026-10-02).

Project it on the real pumpkins, take a photo, and every point on a pumpkin reads off
as a projector pixel. Joe: "boxes tiny for the best resolution", then "2x smaller" ->
10 px boxes (~0.5 % of the width), brighter lines every 50 px (count without losing your
place), heavy lines every 100 px with a small label in each 100 px block's corner
(column letter A..T, row number 1..11), pixel numbers on all four edges. Those numbers
become the face ovals for build_card.py. show.json "loop_n": 163 loops it on the card.

    python3 make_test_grid.py      # -> Main 2026/Calibration/ (picked up by build_card.py as TEST clips)

Two clips, 10 min each (show.json loop_n / LOOP <n> repeats one; PLAY <n> NOW plays it once):
    Test grid.mp4               the grid
    Test grid + faces now.mp4   the grid + the three face circles the current videos use (red)
    Test grid + ovals.mp4       the grid + the three pumpkin ovals read off Joe's photos (green) -- #165
"""
import os, subprocess
from PIL import Image, ImageDraw, ImageFont

W, H = 1920, 1080
MAJOR, MID, MINOR = 100, 50, 10
OUT = "/mnt/media/Pumpkin folder 2026/Main 2026/Calibration"
SECONDS = 600
# Where the faces sit today (build_card.py iris transitions: centre + radius, measured from all face sets)
FACES_NOW = [((344, 622), 279, "L"), ((1010, 586), 263, "C"), ((1628, 620), 196, "R")]
from face_fit import OVALS   # pumpkin outlines (projector px), read off Joe's grid photos 2026-10-02
CELL_COLOURS = [(0, 255, 255), (255, 0, 255), (255, 255, 0)]   # neighbours always differ


def font(size):
    for f in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def grid(with_faces, with_ovals=False):
    im = Image.new("RGB", (W, H), "black")
    d = ImageDraw.Draw(im)
    for x in range(0, W + 1, MINOR):
        d.line([(x, 0), (x, H)], fill=(90, 90, 90), width=1)
    for y in range(0, H + 1, MINOR):
        d.line([(0, y), (W, y)], fill=(90, 90, 90), width=1)
    for x in range(0, W + 1, MID):
        d.line([(x, 0), (x, H)], fill=(170, 170, 170), width=2)
    for y in range(0, H + 1, MID):
        d.line([(0, y), (W, y)], fill=(170, 170, 170), width=2)
    for x in range(0, W + 1, MAJOR):
        d.line([(x, 0), (x, H)], fill=(255, 255, 255), width=3)
    for y in range(0, H + 1, MAJOR):
        d.line([(0, y), (W, y)], fill=(255, 255, 255), width=3)
    big, small = font(17), font(16)
    for ci, x in enumerate(range(0, W, MAJOR)):
        for ri, y in enumerate(range(0, H, MAJOR)):
            lab = f"{chr(65 + ci)}{ri + 1}"
            c = CELL_COLOURS[(ci + ri) % 3]
            d.rectangle([x + 3, y + 3, x + 39, y + 19], fill="black")   # label sits in the corner box
            d.text((x + 5, y + 3), lab, fill=c, font=big)
    for x in range(MAJOR, W, MAJOR):          # pixel numbers on all four edges
        for y, a in ((4, "mt"), (H - 4, "mb")):
            d.text((x, y), str(x), fill=(255, 140, 0), font=small, anchor=a, stroke_width=3, stroke_fill="black")
    for y in range(MAJOR, H, MAJOR):
        for x, a in ((6, "lm"), (W - 6, "rm")):
            d.text((x, y), str(y), fill=(255, 140, 0), font=small, anchor=a, stroke_width=3, stroke_fill="black")
    cx, cy = W // 2, H // 2                   # centre cross
    d.line([(cx - 40, cy), (cx + 40, cy)], fill=(0, 255, 0), width=5)
    d.line([(cx, cy - 40), (cx, cy + 40)], fill=(0, 255, 0), width=5)
    if with_faces:
        for (x, y), r, name in FACES_NOW:
            d.ellipse([x - r, y - r, x + r, y + r], outline=(255, 0, 0), width=3)
            d.line([(x - 25, y), (x + 25, y)], fill=(255, 0, 0), width=6)
            d.line([(x, y - 25), (x, y + 25)], fill=(255, 0, 0), width=6)
            d.text((x, y - r - 6), f"face {name} now", fill=(255, 0, 0), font=font(30), anchor="mb",
                   stroke_width=3, stroke_fill="black")
    if with_ovals:
        for (x, y), a, b, name in OVALS:
            d.ellipse([x - a, y - b, x + a, y + b], outline=(0, 255, 0), width=4)
            d.line([(x - 20, y), (x + 20, y)], fill=(0, 255, 0), width=5)
            d.line([(x, y - 20), (x, y + 20)], fill=(0, 255, 0), width=5)
            d.text((x, y + b + 8), name, fill=(0, 255, 0), font=font(30), anchor="mt",
                   stroke_width=3, stroke_fill="black")
    return im


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, faces, ovals in (("Test grid", False, False), ("Test grid + faces now", True, False),
                               ("Test grid + ovals", False, True)):
        png = os.path.join(OUT, name + ".png")
        mp4 = os.path.join(OUT, name + ".mp4")
        grid(faces, ovals).save(png)
        if os.path.exists(mp4) and os.path.getmtime(mp4) > os.path.getmtime(__file__):
            continue
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-loop", "1", "-framerate", "30", "-i", png,
                        "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-t", str(SECONDS),
                        "-c:v", "libx264", "-tune", "stillimage", "-preset", "veryfast", "-pix_fmt", "yuv420p",
                        "-profile:v", "high", "-level", "4.1", "-c:a", "aac", "-b:a", "64k", mp4], check=True)
        print("wrote", mp4)


if __name__ == "__main__":
    main()
