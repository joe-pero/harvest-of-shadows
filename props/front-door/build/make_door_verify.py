#!/usr/bin/env python3
"""Fast verify still (no ffmpeg warp): the test pattern cropped to ASPECT and homography-warped onto the corners
(same maths as make_door_fitted.sh's perspective filter) + a green outline on the corners. Same env vars.
Writes `SD Card 2026 - calibration/door_grid.mp4` (60 s, the calibration autorun loops it)."""
import os, subprocess, sys
import numpy as np, cv2
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "sdcard"))
import make_door_check as chk, make_door_grid as g   # noqa: E402

e = lambda k, d: int(os.environ.get(k, d))
q = np.float32([[e("TLX", 487), e("TLY", 400)], [e("TRX", 1013), e("TRY", 438)],      # defaults = live fit 10-27
                [e("BRX", 1034), e("BRY", 1417)], [e("BLX", 527), e("BLY", 1478)]])  # (same as make_door_fitted.sh)
asp = float(os.environ.get("ASPECT", 0.433)) or \
    ((q[1, 0] - q[0, 0] + q[2, 0] - q[3, 0]) / 2) / ((q[3, 1] - q[0, 1] + q[2, 1] - q[1, 1]) / 2)
src = cv2.cvtColor(np.array(chk.check_pattern()), cv2.COLOR_RGB2BGR)
cw, ch = (1080, int(1080 / asp)) if 1080 / asp <= 1920 else (int(1920 * asp), 1920)
x0, y0 = (1080 - cw) // 2, (1920 - ch) // 2
crop = src[y0:y0 + ch, x0:x0 + cw]
H = cv2.getPerspectiveTransform(np.float32([[0, 0], [cw, 0], [cw, ch], [0, ch]]), q)
out = cv2.warpPerspective(crop, H, (1080, 1920), flags=cv2.INTER_LANCZOS4)
cv2.polylines(out, [q.astype(np.int32)], True, (0, 255, 0), 6)
# fit checks: diagonals (cross at the door centre), centre lines (edge midpoints; should read plumb/level on the
# door), and each edge's angle in projector px (0 = level / 90 = plumb)
P = lambda a: tuple(int(round(v)) for v in a)
cv2.line(out, P(q[0]), P(q[2]), (255, 255, 0), 2); cv2.line(out, P(q[1]), P(q[3]), (255, 255, 0), 2)
mid = [(q[i] + q[(i + 1) % 4]) / 2 for i in range(4)]       # top, right, bottom, left
cv2.line(out, P(mid[0]), P(mid[2]), (0, 0, 255), 3); cv2.line(out, P(mid[1]), P(mid[3]), (0, 0, 255), 3)
for i, (nm, off) in enumerate((("top", (-60, 45)), ("right", (-190, 0)), ("bottom", (-70, -20)), ("left", (20, 0)))):
    d = q[(i + 1) % 4] - q[i]
    ang = np.degrees(np.arctan2(d[1], d[0])) % 180
    cv2.putText(out, "%s %.1f deg" % (nm, ang), P(mid[i] + off), cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 0), 2)
for (x, y), a in zip(q, ("TL", "TR", "BR", "BL")):
    cv2.putText(out, "%s %d,%d" % (a, x, y), (int(min(x, 900)) - (0 if a[1] == "L" else 160), int(y) + (-12 if a[0] == "T" else 40)),
                cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 0), 2)
up = cv2.cvtColor(out, cv2.COLOR_BGR2RGB)
from PIL import Image
png = "/mnt/media/Front Door 2026/Calibration/verify.png"
g.to_file(Image.fromarray(up)).save(png)
Image.fromarray(up).save("/mnt/media/Front Door 2026/Calibration/verify (street view).png")
subprocess.run(["ffmpeg", "-v", "error", "-y", "-loop", "1", "-framerate", "30", "-i", png, "-f", "lavfi",
                "-i", "anullsrc=r=48000:cl=stereo", "-t", "60", "-c:v", "libx264", "-tune", "stillimage", "-preset",
                "veryfast", "-pix_fmt", "yuv420p", "-profile:v", "high", "-level", "4.2", "-c:a", "aac", "-b:a", "64k",
                "/mnt/media/Front Door 2026/SD Card 2026 - calibration/door_grid.mp4"], check=True)
print("verify still: corners", q.astype(int).tolist(), "aspect %.3f crop %dx%d" % (asp, cw, ch))
