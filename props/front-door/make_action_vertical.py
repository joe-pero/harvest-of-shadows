#!/usr/bin/env python3
"""Turn a 16:9 action clip into the door-projection format.

Door clips are 1080x1920 portrait scenes stored rotated 90 deg CCW inside a
1920x1080 frame (rotate them clockwise to view upright).  This script builds an
upright 1080x1920 frame per source frame, then ffmpeg rotates it CCW and muxes
the original audio.

Per-frame modes (from SECTIONS, by time):
  crop:X  -> 608x1080 slice starting at source x=X, scaled to fill the door
  fit     -> whole 16:9 frame centred, blurred zoomed copy fills above/below
Optional credit removal (CLEAN windows): saturated-red text is masked, the mask
is max-held over +/-HOLD frames so fading text is caught, then inpainted.

usage: make_action_vertical.py rotld|cotc OUT.mp4
  e.g. make_action_vertical.py rotld "/mnt/media/Front Door 2026/SD Card 2026/ROTLD_action_door.mp4"
"""
import subprocess, sys
import cv2
import numpy as np

BASE = "/mnt/media/Front Door 2026/Main 2026"   # sources (never modified)

W, H = 1920, 1080          # source
PW, PH = 1080, 1920        # upright portrait
CW = 608                   # crop width for 9:16 out of 1080 tall
CENTER = (W - CW) // 2     # 656
FPS = 30
HOLD = 20                  # frames each side (~0.67 s) for mask hold

CLIPS = {
    "rotld": dict(
        src=BASE + "/Action/ROTLD_action.avi",
        # (start_s, end_s, mode)
        sections=[(84.0, 94.5, "fit")],
        # keep the big title (84.0-94.5); remove every other credit
        clean=[(72.0, 84.0), (94.5, 999)],
    ),
    "cotc": dict(
        src=BASE + "/Action/Childern_action.avi",
        sections=[
            (0.0, 10.0, "fit"),            # Gatlin production + title card
            (125.4, 132.8, "fit"),         # red title card
            (140.6, 171.27, "crop:286"),   # cross shot: cross is left of centre
            (172.6, 999, "fit"),           # end title card
        ],
        clean=[],
    ),
}


def mode_at(t, sections):
    for a, b, m in sections:
        if a <= t < b:
            return m
    return "crop:%d" % CENTER


def in_windows(t, wins):
    return any(a <= t < b for a, b in wins)


def red_mask(crop):
    b, g, r = [c.astype(np.int16) for c in cv2.split(crop)]
    return ((r - g > 35) & (r - b > 25) & (r > 55)).astype(np.uint8)


def to_portrait_crop(frame, x):
    return cv2.resize(frame[:, x:x + CW], (PW, PH), interpolation=cv2.INTER_LANCZOS4)


def to_portrait_fit(frame):
    bg = cv2.resize(frame[:, CENTER:CENTER + CW], (PW // 4, PH // 4), interpolation=cv2.INTER_AREA)
    bg = cv2.GaussianBlur(bg, (0, 0), 12)
    bg = cv2.resize(bg, (PW, PH), interpolation=cv2.INTER_LINEAR)
    bg = (bg * 0.55).astype(np.uint8)
    fh = PW * H // W  # 607
    fg = cv2.resize(frame, (PW, fh), interpolation=cv2.INTER_AREA)
    y = (PH - fh) // 2
    bg[y:y + fh] = fg
    return bg


def read_frames(src):
    p = subprocess.Popen(["ffmpeg", "-v", "error", "-i", src, "-f", "rawvideo",
                          "-pix_fmt", "bgr24", "-"], stdout=subprocess.PIPE)
    n = W * H * 3
    while True:
        buf = p.stdout.read(n)
        if len(buf) < n:
            break
        yield np.frombuffer(buf, np.uint8).reshape(H, W, 3)
    p.wait()


def main():
    name, out = sys.argv[1], sys.argv[2]
    cfg = CLIPS[name]
    src = cfg["src"]

    # pass 1: per-frame red-text masks (crop region, quarter res) in clean windows
    masks = {}
    if cfg["clean"]:
        for i, f in enumerate(read_frames(src)):
            t = i / FPS
            if in_windows(t, cfg["clean"]):
                x = int(mode_at(t, cfg["sections"]).split(":")[1])
                m = red_mask(f[:, x:x + CW])
                masks[i] = m.reshape(H // 4, 4, CW // 4, 4).max(axis=(1, 3)) > 0  # 4x4 max-pool
        print("pass 1 done: %d masked frames" % len(masks), file=sys.stderr)

    enc = subprocess.Popen([
        "ffmpeg", "-v", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "bgr24", "-s", "%dx%d" % (PW, PH), "-r", str(FPS), "-i", "-",
        "-i", src, "-map", "0:v", "-map", "1:a",
        "-vf", "transpose=2",
        "-c:v", "libx264", "-preset", "slow", "-crf", "18", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "192k", "-shortest", "-movflags", "+faststart", out,
    ], stdin=subprocess.PIPE)

    kernel = np.ones((5, 5), np.uint8)
    for i, f in enumerate(read_frames(src)):
        t = i / FPS
        mode = mode_at(t, cfg["sections"])
        if mode == "fit":
            pf = to_portrait_fit(f)
        else:
            x = int(mode.split(":")[1])
            crop = f[:, x:x + CW].copy()
            if i in masks:
                held = np.zeros_like(masks[i])
                for j in range(i - HOLD, i + HOLD + 1):
                    if j in masks:
                        held |= masks[j]
                if held.any():
                    m = cv2.dilate(held.astype(np.uint8) * 255, kernel, iterations=2)
                    m = cv2.resize(m, (CW, H), interpolation=cv2.INTER_NEAREST)
                    m = cv2.dilate(m, np.ones((3, 3), np.uint8), iterations=2)
                    crop = cv2.inpaint(crop, m, 6, cv2.INPAINT_TELEA)
            pf = to_portrait_crop(crop, 0)
        enc.stdin.write(pf.tobytes())
        if i % 900 == 0:
            print("frame %d (%.0fs)" % (i, t), file=sys.stderr)
    enc.stdin.close()
    sys.exit(enc.wait())


if __name__ == "__main__":
    main()
