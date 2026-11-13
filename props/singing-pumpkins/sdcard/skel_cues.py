#!/usr/bin/env python3
"""Skeleton jaw cues for the Singing Pumpkins (Joe 2026-10-02).

Two 5 V talking skeletons sit with the pumpkins. Power on = jaw opens, power off =
jaw drops with a CLUNK. The pumpkin ESP32 switches them (2-ch relay, GPIO25 left /
GPIO26 right). They are a rare extra, not lip sync: "loud isolated peak ... extra
spice of life in that moment".

This script scans each clip's AUDIO ONLY (no image analysis) and writes candidate
moments; the card sends them to the ESP32 as `CUES <n> <groups>` right after `NOW`,
and the ESP32 decides (rarely, at random) whether to use one.

Moment types (one "group" each, space separated, best first):
    H<c><start>-<end>     hold: jaw open for a peak, clunk when it ends.
                          c = B (both: very top of the song in both channels)
                              L / R (that channel is clearly louder at its peak)
    K<c><t1>,<t2>,...     clack: a repeating pattern of isolated hits (Addams Family
                          snaps); the jaw opens just before each hit and the power cut
                          lands the clunk ON the hit.
Times are seconds from the clip's start.

    python3 skel_cues.py                 # analyze the staged card, update cache + card library.json
    python3 skel_cues.py --show <id>     # print one clip's analysis
    python3 skel_cues.py --stats         # summary over the library (tuning)

Cache: skel_cues.json (repo, keyed by clip id, redone when the staged file changes).
Hand picks: skel_overrides.json  {"<id>": "<groups>" | ""}   ("" = never on this clip)
"""
import json, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CARD = "/mnt/media/Pumpkin folder 2026/SD Card 2026"
CACHE = os.path.join(HERE, "skel_cues.json")
OVERRIDES = os.path.join(HERE, "skel_overrides.json")
ALGO = 1                                   # bump to redo every clip

SKIP_CATS = {"transition", "buffer", "test"}
EDGE_S = 10.0          # ignore the first / last 10 s (loud intros, endings)
SR = 16000

# ---- hold (loud peak) ----
H_FRAME = 0.1          # s
H_SMOOTH = 1.0         # s moving average = "this moment" loudness
H_TOP_PCT = 98.5       # a peak must reach the song's top 1.5 % ...
H_PROM_DB = 3.0        # ... and stand this far above the 20 s around it ("isolated")
H_SIDE_DB = 3.0        # one channel this much louder at the peak = that side only
H_MIN_S, H_MAX_S = 1.0, 4.0
H_SPACING_S = 20.0
H_MAX = 2

# ---- clack (hit pattern) ----
K_HOP = 0.01
K_RISE_DB = 18.0       # hit above the 30-250 ms before it
K_FALL_DB = 15.0       # and the 80-250 ms after it (a short, isolated hit)
K_MERGE_S = 0.10       # double detections
K_MIN_GAP_S = 0.35     # the jaw cannot clack faster than this
K_RUN_GAP_S = 4.0      # hits further apart = separate runs
K_EDGE_S = 1.0        # snaps in an intro are fine (Addams Family opens with them)
K_GAP_TOL_S = 0.05
K_MIN_PAIRS = 3        # the same gap at least 3 times in one run = a pattern, not chance
K_MAX_HITS = 8         # one moment = up to 8 clunks
K_MAX = 1


def load(path, af=None, ch=2):
    cmd = ["ffmpeg", "-v", "error", "-i", path]
    if af:
        cmd += ["-af", af]
    cmd += ["-ac", str(ch), "-ar", str(SR), "-f", "s16le", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.int16).astype(np.float32).reshape(-1, ch) / 32768


def frame_db(x, hop):
    n = len(x) // hop
    p = (x[:n * hop].reshape(n, hop, -1) ** 2).mean(1)
    return p  # power per channel


def smooth(v, k):
    return np.convolve(v, np.ones(k) / k, mode="same")


def holds(path):
    x = load(path)
    hop = int(SR * H_FRAME)
    p = frame_db(x, hop)
    k = int(H_SMOOTH / H_FRAME)
    pl, pr = smooth(p[:, 0], k), smooth(p[:, 1], k)
    db = lambda v: 10 * np.log10(v + 1e-10)
    mid, L, R = db((pl + pr) / 2), db(pl), db(pr)
    n = len(mid)
    edge = int(EDGE_S / H_FRAME)
    if n < 2 * edge + 10:
        return []
    top = np.percentile(mid[edge:n - edge], H_TOP_PCT)
    ctx = int(10 / H_FRAME)
    out = []
    used = np.zeros(n, bool)
    for i in np.argsort(mid)[::-1]:
        if len(out) >= H_MAX or mid[i] < top:
            break
        if i < edge or i >= n - edge or used[max(0, i - int(H_SPACING_S / H_FRAME)):i + int(H_SPACING_S / H_FRAME)].any():
            continue
        around = np.concatenate([mid[max(0, i - ctx):max(0, i - k)], mid[i + k:i + ctx]])
        prom = mid[i] - np.median(around) if len(around) else 0
        if prom < H_PROM_DB:
            continue
        # window = where it stays within 1.5 dB of the peak, H_MIN_S..H_MAX_S
        a = b = i
        lim = mid[i] - 1.5
        while a > 0 and mid[a - 1] >= lim and (b - a) * H_FRAME < H_MAX_S:
            a -= 1
        while b < n - 1 and mid[b + 1] >= lim and (b - a) * H_FRAME < H_MAX_S:
            b += 1
        while (b - a) * H_FRAME < H_MIN_S:
            a, b = max(0, a - 1), min(n - 1, b + 1)
        d = float(np.mean(L[a:b + 1] - R[a:b + 1]))
        c = "L" if d >= H_SIDE_DB else "R" if d <= -H_SIDE_DB else "B"
        used[a:b + 1] = True
        out.append(dict(c=c, a=round(a * H_FRAME, 1), b=round(b * H_FRAME, 1),
                        prom=round(float(prom), 1), side=round(d, 1)))
    return out


def hits(path):
    x = load(path, af="highpass=f=1500")
    hop = int(SR * K_HOP)
    p = frame_db(x, hop)
    db = 10 * np.log10(p + 1e-10)                      # n x 2
    m = 10 * np.log10(p.mean(1) + 1e-10)
    n = len(m)
    out = []
    for i in range(30, n - 30):
        if m[i] != m[i - 3:i + 4].max():
            continue
        rise = m[i] - np.median(m[i - 25:i - 3])
        fall = m[i] - np.median(m[i + 8:i + 25])
        if rise >= K_RISE_DB and fall >= K_FALL_DB:
            t = i * K_HOP
            if out and t - out[-1]["t"] < K_MERGE_S:
                continue
            out.append(dict(t=round(t, 2), s=round(float(rise), 1), side=round(float(db[i, 0] - db[i, 1]), 1)))
    return out


def clacks(path, dur):
    """A repeating pair: the same hit-to-hit gap (snap ... snap) at least K_MIN_PAIRS times
    in one run. Only the hits in those pairs are kept, so stray hits in between don't count."""
    hs = [h for h in hits(path) if K_EDGE_S <= h["t"] <= dur - K_EDGE_S]
    runs, cur = [], []
    for h in hs:
        if cur and h["t"] - cur[-1]["t"] > K_RUN_GAP_S:
            runs.append(cur)
            cur = []
        if cur and h["t"] - cur[-1]["t"] < K_MIN_GAP_S:
            continue
        cur.append(h)
    if cur:
        runs.append(cur)
    good = []
    for r in runs:
        ts = [h["t"] for h in r]
        gaps = np.diff(ts)
        best = []
        for g in gaps:
            pairs = [i for i, x in enumerate(gaps) if abs(x - g) <= K_GAP_TOL_S]
            if len(pairs) > len(best):
                best = pairs
        if len(best) < K_MIN_PAIRS:
            continue
        keep = sorted({i for p in best for i in (p, p + 1)})[:K_MAX_HITS]
        sel = [r[i] for i in keep]
        side = float(np.mean([h["side"] for h in sel]))
        c = "L" if side >= H_SIDE_DB else "R" if side <= -H_SIDE_DB else "B"
        good.append(dict(c=c, t=[h["t"] for h in sel], gap=round(float(np.median(gaps[best])), 2),
                         score=round(sum(h["s"] for h in sel), 1)))
    good.sort(key=lambda g: -g["score"])
    return good[:K_MAX]


def analyze(it):
    path = os.path.join(CARD, it["file"])
    hs = holds(path)
    ks = clacks(path, it["dur"])
    return dict(algo=ALGO, size=os.path.getsize(path), mtime=int(os.path.getmtime(path)), holds=hs, clacks=ks)


def groups(entry):
    """Best first: a clack pattern (the 'logical' moment) beats a loud peak."""
    g = [f"K{k['c']}" + ",".join(f"{t:.2f}" for t in k["t"]) for k in entry["clacks"]]
    g += [f"H{h['c']}{h['a']:.1f}-{h['b']:.1f}" for h in entry["holds"]]
    s = " ".join(g)
    while len(s) > 180:      # ESP32 line buffer
        g.pop()
        s = " ".join(g)
    return s


def load_cache():
    try:
        with open(CACHE) as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def load_overrides():
    try:
        with open(OVERRIDES) as f:
            return {k: v for k, v in json.load(f).items() if not k.startswith("_")}
    except FileNotFoundError:
        return {}


def eligible(items):
    return [it for it in items if it["category"] not in SKIP_CATS]


def update(items, workers=4):
    """Analyze new/changed clips. Returns {id: groups string}."""
    cache = load_cache()
    todo = []
    for it in eligible(items):
        path = os.path.join(CARD, it["file"])
        c = cache.get(it["id"])
        if not (c and c.get("algo") == ALGO and c.get("size") == os.path.getsize(path)
                and c.get("mtime") == int(os.path.getmtime(path))):
            todo.append(it)
    if todo:
        print(f"[skel] analyzing {len(todo)} clips", flush=True)
        with ThreadPoolExecutor(workers) as ex:
            for it, e in zip(todo, ex.map(analyze, todo)):
                cache[it["id"]] = e
        with open(CACHE, "w") as f:
            json.dump(cache, f, indent=1, sort_keys=True)
    ov = load_overrides()
    return {it["id"]: ov.get(it["id"], groups(cache[it["id"]])) for it in eligible(items)}


def main():
    with open(os.path.join(CARD, "library.json")) as f:
        lib = json.load(f)
    items = lib["items"]
    if "--show" in sys.argv:
        want = sys.argv[sys.argv.index("--show") + 1]
        it = next(i for i in items if i["id"] == want)
        e = analyze(it)
        print(json.dumps(e, indent=1))
        print("CUES", groups(e))
        print("hits:", " ".join(f"{h['t']}" for h in hits(os.path.join(CARD, it["file"]))))
        return
    cues = update(items)
    for it in items:
        it.pop("skel", None)
        if cues.get(it["id"]):
            it["skel"] = cues[it["id"]]
    with open(os.path.join(CARD, "library.json"), "w") as f:
        json.dump(lib, f, indent=1)
    by = {}
    for it in eligible(items):
        s = cues.get(it["id"], "")
        k = "clack" if " " + "K" in " " + s else "hold" if s else "none"
        by.setdefault((it["family"], it["category"]), {}).setdefault(k, 0)
        by[(it["family"], it["category"])][k] += 1
    for k, v in sorted(by.items()):
        print(f"{k[0]:5s} {k[1]:12s} {v}")
    if "--stats" in sys.argv:
        for it in eligible(items):
            print(f"{it['n'] or '':>3} {it['id'][:40]:40s} {cues.get(it['id'], '')}")
    print("library.json updated:", sum(1 for i in items if i.get("skel")), "clips with cues")


if __name__ == "__main__":
    main()
