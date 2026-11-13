#!/usr/bin/env python3
"""Fit every clip's three faces onto the real pumpkins (Joe 2026-10-02).

The pumpkin outlines come from Joe's grid photos (make_test_grid.py, clip "Test grid +
ovals", confirmed "perfectly" on the real projection). For each clip:

  1. detect():  where each face actually is. Max over the whole clip (1 frame/s): a pixel
     counts when it is lit warm (faces are yellow/orange; skips cyan water, grey smoke,
     white streaks) in >= 1 % of the sampled frames. One box per third of the frame.
  2. fit_filter():  crop each face box, scale it (aspect kept) to FIT of its pumpkin's oval,
     centre it on the oval, then black out everything outside the ovals with a feathered
     mask (also removes TSP backgrounds). Faces keep their size for the whole clip.

Face order: the wide, flat face always goes on the fat LEFT pumpkin (AtmosFX convention,
Joe 2026-10-02). SWAP_LR in build_card.py flips the clips whose sides were swapped.

    python3 face_fit.py <video> [...]     # print the detected boxes (debug)

Boxes are cached in face_boxes.json (repo) keyed by source path + size + mtime.
"""
import hashlib, json, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

W, H = 1920, 1080
# Pumpkin outlines in projector pixels: centre x, y, half-width, half-height, name.
# Read off Joe's grid photos 2026-10-02 (IMG_9926/9927/9931), confirmed on the projection.
OVALS = [((564, 536), 204, 116, "L"), ((942, 537), 130, 145, "C"), ((1278, 592), 113, 97, "R")]
FIT = 0.74         # face box = this fraction of the oval's width/height (box corners stay ~inside)
FEATHER = 16       # px, soft edge of the oval mask
DET_W, DET_H = 480, 270
HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "face_boxes.json")
MASK = os.path.join(HERE, "oval_mask.png")        # RGBA, black outside the ovals (preview/debug)
MASK_A = os.path.join(HERE, "oval_alpha.png")     # grey: 255 inside the ovals, feathered -- the encode input
DET_VERSION = 1    # bump to redo every detection
# Hand-set boxes where warm-pixel detection guesses wrong: (source folder, third) -> box.
# Suno King face is white with yellow eyes, so detection only saw the eyes and cut the jaw
# (wide-open mouth, ~10 % of frames) + brow tips. Lit extent measured over whole clips
# 2026-10-27: y 482-732; width kept = same face size on the pumpkin (cheek curls stay off).
BOX_FIX = {("Custom Suno Pumpkins - Composited", 1): [896, 476, 248, 264]}


def _load():
    try:
        with open(CACHE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def detect(path, cache=None):
    """-> [box or None] * 3 (left, centre, right third), box = [x, y, w, h] in 1920x1080."""
    st = os.stat(path)
    key = os.path.abspath(path)
    own = cache is None
    cache = _load() if own else cache
    c = cache.get(key)
    if c and c["size"] == st.st_size and c["mtime"] == int(st.st_mtime) and c.get("v") == DET_VERSION:
        return _fix(path, c["boxes"])
    raw = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", path, "-vf",
                          f"fps=1,scale={DET_W}:{DET_H}:flags=area,format=rgb24",
                          "-f", "rawvideo", "-"], capture_output=True, check=True).stdout
    fr = np.frombuffer(raw, np.uint8).reshape(-1, DET_H, DET_W, 3).astype(np.int16)
    warm = (fr[..., 0] > 70) & (fr[..., 0] > fr[..., 2] + 40)
    keep = warm.sum(0) >= max(2, int(np.ceil(0.01 * len(fr))))
    k = W // DET_W
    boxes = []
    third = DET_W // 3
    for t in range(3):
        ys, xs = np.nonzero(keep[:, t * third:(t + 1) * third])
        if len(xs) < 30:
            boxes.append(None)
            continue
        x0, x1 = np.percentile(xs, [0.2, 99.8]) + t * third
        y0, y1 = np.percentile(ys, [0.2, 99.8])
        pad = 2  # detection px
        x0 = max(t * third, x0 - pad); x1 = min((t + 1) * third - 1, x1 + pad)
        y0 = max(0, y0 - pad); y1 = min(DET_H - 1, y1 + pad)
        boxes.append([int(x0 * k), int(y0 * k), int((x1 - x0 + 1) * k), int((y1 - y0 + 1) * k)])
    cache[key] = dict(size=st.st_size, mtime=int(st.st_mtime), v=DET_VERSION, frames=len(fr), boxes=boxes)
    if own:
        save(cache)
    return _fix(path, boxes)


def _fix(path, boxes):
    folder = os.path.basename(os.path.dirname(os.path.abspath(path)))
    return [BOX_FIX.get((folder, t), b) for t, b in enumerate(boxes)]


def save(cache):
    with open(CACHE, "w") as f:
        json.dump(cache, f, indent=1, sort_keys=True)


def assign(boxes, swap=False):
    """Face box per oval (left, centre, right)."""
    return [boxes[2], boxes[1], boxes[0]] if swap else list(boxes)


def placements(assigned):
    """-> [(crop x, y, w, h, scaled W, H, overlay x, y)] for the faces that exist."""
    out = []
    for box, ((cx, cy), a, b, _) in zip(assigned, OVALS):
        if not box:
            continue
        x, y, w, h = box
        s = FIT * min(2 * a / w, 2 * b / h)
        sw, sh = max(2, int(w * s / 2) * 2), max(2, int(h * s / 2) * 2)
        out.append((x, y, w, h, sw, sh, cx - sw // 2, cy - sh // 2))
    return out


def make_mask():
    """RGBA overlay: transparent inside the ovals (feathered), black outside."""
    m = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(m)
    for (cx, cy), a, b, _ in OVALS:
        e = FEATHER * 0.6
        d.ellipse([cx - a + e, cy - b + e, cx + a - e, cy + b - e], fill=255)
    m = m.filter(ImageFilter.GaussianBlur(FEATHER / 2.5))
    m.save(MASK_A)
    alpha = Image.eval(m, lambda v: 255 - v)
    im = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    im.putalpha(alpha)
    im.save(MASK)
    return MASK


def fit_filter(assigned, inp="0:v", mask="1:v", out="v"):
    """filter_complex string: faces into the ovals, black outside. `mask` = the MASK_A image input
    (one frame, repeated). The oval edge is applied to each small face crop only (alphamerge), not
    to the whole frame: ~3x faster than a full-frame mask overlay (benchmarked 2026-10-02)."""
    pl = placements(assigned)
    n = len(pl)
    fc = [f"[{inp}]scale={W}:{H},setsar=1,split={n + 1}[bgs]" + "".join(f"[f{i}]" for i in range(n)),
          "[bgs]drawbox=c=black:t=fill[b0]"]
    if n:
        fc.append(f"[{mask}]format=gray,split={n}" + "".join(f"[m{i}]" for i in range(n)))
    last = "b0"
    for i, (x, y, w, h, sw, sh, ox, oy) in enumerate(pl):
        fc.append(f"[m{i}]crop={sw}:{sh}:{ox}:{oy}[mc{i}]")
        fc.append(f"[f{i}]crop={w}:{h}:{x}:{y},scale={sw}:{sh}:flags=lanczos,format=yuva420p[s{i}]")
        fc.append(f"[s{i}][mc{i}]alphamerge[a{i}]")
        fc.append(f"[{last}][a{i}]overlay=x={ox}:y={oy}[b{i + 1}]")
        last = f"b{i + 1}"
    fc.append(f"[{last}]format=yuv420p[{out}]")
    return ";".join(fc)


def fit_tag(assigned, src):
    """Short hash of everything that changes the picture; stored in the staged file's comment."""
    s = json.dumps([assigned, OVALS, FIT, FEATHER, os.path.abspath(src)], sort_keys=True)
    return "fit=" + hashlib.md5(s.encode()).hexdigest()[:10]


if __name__ == "__main__":
    c = _load()
    for p in sys.argv[1:]:
        print(detect(p, c), p)
    save(c)
