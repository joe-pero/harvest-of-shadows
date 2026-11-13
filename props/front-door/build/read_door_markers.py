#!/usr/bin/env python3
"""Read the door corners off one photo of the ArUco calibration card (make_door_markers.py).

  1. detect the markers -> exact photo <-> projector-pixel pairs (street-view raster 1080x1920); RANSAC drops
     markers off the door plane (side brick)
  2. find the white cloth: detected markers painted in as cloth + pixels brighter than 1/3 of the cloth's own
     white (shaded cloth passes, the black tape does not) + a SMALL closing (a big one jumped the tape onto
     whitewashed brick, 10-27); the blob holding the most markers; each side fitted to the OUTER envelope of
     the outline (unread edge markers only notch it inward) -> 4 corners in the photo
  3. each corner -> projector px through a homography fitted to the 12 markers NEAREST that corner
     (handles cloth wrinkles / a non-flat door better than one global fit)
  4. the cloth's true width/height from the photo quad + the iPhone's EXIF focal length
     (single-view rectangle aspect, Zhang & He), used to crop the clips so nothing is stretched
Proven 2026-10-27: two walkway photos agreed to within 2-18 projector px and 0.001 in aspect.
Writes a debug overlay next to the photo (scratch dir given) and prints the env line for make_door_fitted.sh.

    python3 read_door_markers.py PHOTO.jpg [OUTDIR] [--inset PX]
"""
import os, sys
import numpy as np, cv2
from PIL import Image, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_door_markers as m   # noqa: E402


def load(path):
    pil = Image.open(path)
    ex = pil.getexif().get_ifd(0x8769)
    f35 = ex.get(41989)                       # FocalLengthIn35mmFilm
    im = cv2.cvtColor(np.array(ImageOps.exif_transpose(pil).convert("RGB")), cv2.COLOR_RGB2BGR)
    return im, f35


def detect(im):
    p = cv2.aruco.DetectorParameters()
    p.cornerRefinementMethod = cv2.aruco.CORNER_REFINE_SUBPIX
    det = cv2.aruco.ArucoDetector(cv2.aruco.getPredefinedDictionary(m.DICT), p)
    corners, ids, _ = det.detectMarkers(cv2.cvtColor(im, cv2.COLOR_BGR2GRAY))
    if ids is None:
        sys.exit("no markers found")
    L = m.layout()
    ph, pj, ctr = [], [], []
    for q, i in zip(corners, ids.ravel()):
        if int(i) in L:
            ph.append(q[0]); pj.append(L[int(i)]); ctr.append(q[0].mean(0))
    return np.array(ph), np.array(pj), np.array(ctr)


def cloth_mask(im, ph, ctr):
    """White cloth blob. Detected markers are painted in as cloth (no big closing that could jump the black
    tape onto whitewashed brick); brightness cut = half the cloth's own white level."""
    hsv = cv2.cvtColor(im, cv2.COLOR_BGR2HSV)
    dd = np.linalg.norm(ctr[:, None] - ctr[None], axis=2); np.fill_diagonal(dd, np.inf)
    pitch = np.median(dd.min(1))
    hull = np.zeros(im.shape[:2], np.uint8)
    cv2.fillConvexPoly(hull, cv2.convexHull(ctr.astype(np.int32)), 255)
    v = hsv[..., 2][(hull > 0) & (hsv[..., 1] < 60)]
    thr = 0.33 * np.percentile(v, 90)                     # shaded cloth still clears it; the black tape does not
    white = ((hsv[..., 2] > thr) & (hsv[..., 1] < 70)).astype(np.uint8) * 255
    for q in ph:                                            # markers = cloth
        c0 = q.mean(0)
        cv2.fillConvexPoly(white, ((q - c0) * 1.2 + c0).astype(np.int32), 255)
    k = max(5, int(pitch * 0.15))
    pad = 2 * k
    white = cv2.copyMakeBorder(white, pad, pad, pad, pad, cv2.BORDER_CONSTANT, value=0)
    white = cv2.morphologyEx(white, cv2.MORPH_CLOSE, cv2.getStructuringElement(cv2.MORPH_RECT, (k, k)))
    white = cv2.morphologyEx(white, cv2.MORPH_OPEN, cv2.getStructuringElement(cv2.MORPH_RECT, (2 * k, 2 * k)))
    white = white[pad:-pad, pad:-pad]
    n, lab = cv2.connectedComponents(white)
    votes = np.bincount(lab[ctr[:, 1].astype(int).clip(0, im.shape[0] - 1), ctr[:, 0].astype(int).clip(0, im.shape[1] - 1)],
                        minlength=n)
    votes[0] = 0
    blob = (lab == votes.argmax()).astype(np.uint8) * 255
    cs, _ = cv2.findContours(blob, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)   # fill holes
    out = np.zeros_like(blob); cv2.drawContours(out, cs, -1, 255, -1)
    return out


def quad(mask):
    """4 corners (TL, TR, BR, BL) of the cloth: line fit per side of the outline, intersections."""
    cs, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    c = max(cs, key=cv2.contourArea).reshape(-1, 2).astype(float)
    rough = cv2.approxPolyDP(c.astype(np.float32), 0.02 * cv2.arcLength(c.astype(np.float32), True), True).reshape(-1, 2)
    if len(rough) < 4:
        sys.exit("cloth outline not 4-sided")
    hull = cv2.convexHull(rough.astype(np.float32)).reshape(-1, 2)
    s, d = hull.sum(1), hull[:, 0] - hull[:, 1]
    rc = [hull[s.argmin()], hull[d.argmax()], hull[s.argmax()], hull[d.argmin()]]   # TL TR BR BL
    lines = []
    for a, b in zip(rc, rc[1:] + rc[:1]):
        # contour points near segment a-b, away from the corners (rounded corners / tape)
        ab = b - a; L2 = ab @ ab
        t = ((c - a) @ ab) / L2
        dist = np.abs(ab[0] * (c[:, 1] - a[1]) - ab[1] * (c[:, 0] - a[0])) / np.sqrt(L2)
        sel = (t > 0.1) & (t < 0.9) & (dist < 0.08 * np.sqrt(L2))
        # outer envelope: per slice along the side keep the point furthest OUT (notches from unread
        # edge markers / dark marker squares can only pull the outline inward)
        nrm = np.array([ab[1], -ab[0]]) / np.sqrt(L2)          # outward for a clockwise TL->TR->BR->BL walk
        off = (c - a) @ nrm
        pts = []
        for b0 in np.linspace(0.1, 0.9, 41)[:-1]:
            k = sel & (t >= b0) & (t < b0 + 0.02)
            if k.any():
                pts.append(c[k][off[k].argmax()])
        pts = np.array(pts)
        vx, vy, x0, y0 = cv2.fitLine(pts.astype(np.float32), cv2.DIST_HUBER, 0, 0.01, 0.01).ravel()
        lines.append((np.array([x0, y0]), np.array([vx, vy])))
    out = []
    for (p1, d1), (p2, d2) in zip(lines[-1:] + lines[:-1], lines):       # side before x side after
        A = np.array([d1, -d2]).T
        t = np.linalg.solve(A, p2 - p1)
        out.append(p1 + t[0] * d1)
    return np.array(out), c


def to_proj(pt, ph, pj, ctr, k=12):
    near = np.argsort(np.linalg.norm(ctr - pt, axis=1))[:k]
    H, _ = cv2.findHomography(ph[near].reshape(-1, 2), pj[near].reshape(-1, 2), cv2.RANSAC, 3.0)
    return cv2.perspectiveTransform(np.array([[pt]], np.float32), H)[0, 0]


def true_aspect(q, w, h, f35):
    """width/height of the real rectangle imaged as quad q (TL TR BR BL), pinhole at the image centre."""
    if not f35:
        return None
    f = f35 / 36.0 * max(w, h)                 # 35 mm-equivalent across the long side
    u0, v0 = w / 2, h / 2
    m1, m2, m3, m4 = [np.array([p[0], p[1], 1.0]) for p in (q[3], q[2], q[0], q[1])]   # BL BR TL TR (Zhang & He)
    k2 = np.dot(np.cross(m1, m4), m3) / np.dot(np.cross(m2, m4), m3)
    k3 = np.dot(np.cross(m1, m4), m2) / np.dot(np.cross(m3, m4), m2)
    n2, n3 = k2 * m2 - m1, k3 * m3 - m1
    A = np.array([[f, 0, u0], [0, f, v0], [0, 0, 1]]); Ai = np.linalg.inv(A)
    B = Ai.T @ Ai
    return float(np.sqrt((n2 @ B @ n2) / (n3 @ B @ n3)))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    inset = 0
    if "--inset" in sys.argv:
        inset = int(sys.argv[sys.argv.index("--inset") + 1]); args = args[:-1] if args[-1] == str(inset) else args
    path = args[0]
    outdir = args[1] if len(args) > 1 else os.path.dirname(path)
    im, f35 = load(path)
    ph, pj, ctr = detect(im)
    _, inl = cv2.findHomography(ctr.astype(np.float32), pj.mean(1).astype(np.float32), cv2.RANSAC, 8.0)
    keep = inl.ravel() > 0
    ph, pj, ctr = ph[keep], pj[keep], ctr[keep]
    mask = cloth_mask(im, ph, ctr)
    qp, contour = quad(mask)
    qj = np.array([to_proj(p, ph, pj, ctr) for p in qp])
    # global fit residual = how flat / consistent the marker map is
    Hg, inl = cv2.findHomography(ph.reshape(-1, 2), pj.reshape(-1, 2), cv2.RANSAC, 3.0)
    res = np.linalg.norm(cv2.perspectiveTransform(ph.reshape(-1, 1, 2).astype(np.float32), Hg).reshape(-1, 2) - pj.reshape(-1, 2), axis=1)
    asp = true_aspect(qp, im.shape[1], im.shape[0], f35)
    if inset:                                   # pull each corner toward the centre (projector px)
        cen = qj.mean(0); qj = qj + (cen - qj) / np.linalg.norm(cen - qj, axis=1)[:, None] * inset
    qj = np.round(qj).astype(int)

    print("photo %s  %dx%d  f35=%s mm" % (os.path.basename(path), im.shape[1], im.shape[0], f35))
    print("markers %d of %d (corners %d), global-fit residual median %.1f / 90%% %.1f proj px"
          % (len(ph), m.COLS * m.ROWS, len(ph) * 4, np.median(res), np.percentile(res, 90)))
    for n, p, j in zip(("TL", "TR", "BR", "BL"), qp, qj):
        flag = "" if 0 <= j[0] <= 1080 and 0 <= j[1] <= 1920 else "   <-- OUTSIDE the picture"
        print("  %s photo (%5.0f,%5.0f) -> projector (%4d,%4d)%s" % (n, p[0], p[1], j[0], j[1], flag))
    pw = (np.linalg.norm(qj[1] - qj[0]) + np.linalg.norm(qj[2] - qj[3])) / 2
    phh = (np.linalg.norm(qj[3] - qj[0]) + np.linalg.norm(qj[2] - qj[1])) / 2
    print("cloth in projector px: %.0f x %.0f (w/h %.3f); true w/h from photo: %s"
          % (pw, phh, pw / phh, "%.3f" % asp if asp else "n/a (no EXIF focal length)"))
    print("TLX=%d TLY=%d TRX=%d TRY=%d BRX=%d BRY=%d BLX=%d BLY=%d%s" % (*qj[0], *qj[1], *qj[2], *qj[3],
          (" ASPECT=%.4f" % asp) if asp else ""))

    dbg = im.copy()
    for q in ph:
        cv2.polylines(dbg, [q.astype(np.int32)], True, (255, 160, 0), 3)
    cv2.drawContours(dbg, [contour.astype(np.int32).reshape(-1, 1, 2)], -1, (0, 255, 255), 4)
    cv2.polylines(dbg, [qp.astype(np.int32)], True, (0, 0, 255), 6)
    for n, p in zip(("TL", "TR", "BR", "BL"), qp):
        cv2.circle(dbg, tuple(p.astype(int)), 18, (0, 0, 255), -1)
        cv2.putText(dbg, n, tuple((p + 25).astype(int)), cv2.FONT_HERSHEY_SIMPLEX, 3, (0, 0, 255), 6)
    out = os.path.join(outdir, os.path.splitext(os.path.basename(path))[0] + "_read.jpg")
    cv2.imwrite(out, cv2.resize(dbg, None, fx=0.5, fy=0.5), [cv2.IMWRITE_JPEG_QUALITY, 85])
    print("overlay", out)


if __name__ == "__main__":
    main()
