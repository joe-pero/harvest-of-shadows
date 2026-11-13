#!/usr/bin/env python3
"""Build the Singing Pumpkins BrightSign SD card from the Main 2026 sandbox.

    python3 build_card.py            # scan + stage media + write library.json/TRACKS.md
    python3 build_card.py --dry-run  # scan + print the library, touch nothing
    python3 build_card.py --transitions  # also regenerate the 6 iris transitions

Source (never modified): /mnt/media/Pumpkin folder 2026/Main 2026/
Output (card image):     /mnt/media/Pumpkin folder 2026/SD Card 2026/
    autorun.brs, show.json      copied from this folder (the show logic)
    media/transitions/*.mp4     generated: iris transition per face-group change (6 files)
    library.json                generated: every video, its number, family, category, duration,
                                skeleton jaw cues ("skel", from skel_cues.py)
    TRACKS.md                   generated: human-readable duration table
    media/<family>/<slug>.mp4   staged copies: every face fitted onto its real pumpkin
                                (face_fit.py: oval per pumpkin, feathered mask, re-encoded),
                                audio re-encoded AAC 48 kHz stereo, loudness-normalized
                                to TARGET_LUFS, 0.3 s audio fade in/out
    python3 build_card.py --preview  # one fitted frame per clip -> contact sheets, touch nothing else

Copy the whole "SD Card 2026" folder onto the root of the BrightSign's microSD.
Staging is incremental: a staged file newer than its source is skipped.
"""
import json, os, re, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
import skel_cues
import face_fit

SRC = "/mnt/media/Pumpkin folder 2026/Main 2026"
# TheSingingPumpkin's original downloads (Joe 2026-10-02: "best video quality wins"): ~2 Mbps
# vs ~0.7 for the re-encoded "resized" copies in Main 2026. Same songs, same length.
RAW = "/mnt/media/Remote Media/Projector/Halloween/The Singing Pumpkin"
CACHE_DIR = "/mnt/media/Pumpkin folder 2026/cache"
PREVIEW_DIR = "/mnt/media/Pumpkin folder 2026/Fit preview"
OUT = "/mnt/media/Pumpkin folder 2026/SD Card 2026"
HERE = os.path.dirname(os.path.abspath(__file__))
TARGET_LUFS = -16.0
AFADE = 0.3

# Silent / near-silent clips: loudnorm would pump the noise floor up, so only re-encode.
SILENT_BELOW_LUFS = -35.0

# Joe's buffer / fade clips. Staged onto the card but NOT used by autorun.brs since
# 2026-09-27 (the iris transitions below replace them). role kept for reference.
BRIDGES = {
    "Trick or treaters/Buffer /Intro laughing into the video.mp4": ("ATMOS", "in"),
    "Trick or treaters/Buffer /Exit buffer.mp4":                   ("ATMOS", "out"),
    "Trick or treaters/Buffer /flickering candle.mp4":             ("ATMOS", "idle"),
    "Trick or treaters/Buffer /flickering candle 2.mp4":           ("ATMOS", "idle"),
    "Trick or treaters/Buffer /Looking around.mp4":                ("ATMOS", "idle"),
    "Trick or treaters/Buffer /Pumpkin static flicker Buffer.mp4": ("ATMOS", "idle"),
    "TheSingingpumpkins/TheSingingPumpkins_Transition Files/IN fade.mp4":  ("TSP", "in"),
    "TheSingingpumpkins/TheSingingPumpkins_Transition Files/out fade.mp4": ("TSP", "out"),
}
# The three JOLJ3 buffer folders hold identical copies; take one trio buffer as an ATMOS idle.
ATMOS_TRIO_BUFFER = ("Atmosfx Pumpkins/Jack-O-Lantern AtmosFX/Jack-O-Lantern Main Videos/"
                     "Pumpkin Songs/Jack-O-Lantern Jamboree 3 - Halloween Hits - Pumpkin/"
                     "Extra Buffer Files/JOLJ3_Buffer_Trio_Pumpkin.mp4")


# Main 2026 TSP copy -> raw original (rel to RAW). A list = parts joined end to end.
TSP_RAW = {f"TheSingingpumpkins/{k}": v for k, v in {
    "Resized Addams Family.mp4": "Addams Family/Addams Family 1080HD-16.mp4",
    "Bad Moon Rising.mp4": "Bad Moon Rising/Bad Moon Rising 1080HD-22.mp4",
    "Blue Moon.mp4": "Blue Moon/The Singing Pumpkin - BlueMoon HD -1080p.mp4",  # older all-yellow, = card
    "Bohemian Rhapsody Resized.mp4": "Bohemian Rhapsody/Bohemian Rhapsody 1080HD-19.mp4",
    "Dead Mans Party REsized.mp4": "Dead Man's Party/Dead Mans Party HD - 1080p.mp4",
    "Double Trouble Resized.mp4": "Double Trouble/Double Trouble 1080HD-22.mp4",
    "Enter Sandman REsized.mp4": "Enter Sandman/Enter Sandman 1080HD-21.mp4",
    "Jump In the Line resized.mp4": "Jump in the Line/Jump In the Line HD - 1080p.mp4",
    "Monster Mash.mp4": "Monster Mash/Monster Mash 1080HD-16.mp4",
    "The Singing Pumpkins - Neverending Story resized.mp4":
        "Never Ending Story/The Singing Pumpkins - Neverending Story.mp4",
    "Pirate Pumpkins - Old Maui Resized.mp4": "Rams Horn Studios/Pirate Pumpkins - Old Maui.mp4",
    "Pirate Pumpkins - Randy Dandy-O.mp4": "Rams Horn Studios/Pirate Pumpkins - Randy Dandy-O.mp4",
    "Red Riding Hood HD - Resize.mp4": "Lil Red Riding Hood/Red Riding Hood HD - 1080p.mp4",
    "Riders in the Sky Resized.mp4": "Ghost Riders in the Sky/Riders in the Sky 1080HD-21.mp4",
    "sexy and i know it.mp4": "Sexy and I Know It/Sexy and I Know It 1080HD-16.mp4",
    "shake your bones.mp4": "Shake Your Bones/Shake Your Bones 1080HD-22.mp4",
    "This is halloween.mp4": "This is Halloween/This is Halloween 1080HD-16.mp4",
    "Thriller.mp4": "Thriller Pumpkin/Thriller 1080HD-16.mp4",
    "Thunderstruck.mp4": "Thunderstruck/Thunderstruck 1080HD-19.mp4",
    "Werewolves of london.mp4": "Werewolves of London/The Singing Pumpkin - Werewolves of London HD - 720p.mp4",
    "Zombie Jamboree.mp4": "Zombie Jamborie/The Singing Pumpkin - Zombie Jamboree HD - 1080p.mp4",
    "Ghost host with Gim grinning ghosts resized.mp4": ["Ghost Host/Ghost Host 1080HD-16.mp4",
                                                        "Grin Grinning Ghosts/Grim Grinning Ghosts 1080HD-16.mp4"],
}.items()}
# TSP songs only in the raw folder, added by Joe 2026-10-02 (picked 1, 2, 4 of the list).
EXTRA_TSP = {
    "Ghostbusters": "Ghostbusters/Ghostbusters 1080HD-16.mp4",
    "Grim Grinning Ghosts": "Grin Grinning Ghosts/Grim Grinning Ghosts 1080HD-16.mp4",
    "Trick Ghastly Ghostbusters": "Trick Ghastly/Ghostbusters/03 - Trick Ghastly-Ghostbusters_with Music Added.mp4",
}
# Clips whose side faces are swapped (zigzag face left): flipped so the wide flat face sits
# on the fat left pumpkin like AtmosFX (Joe 2026-10-02). Raw originals are all correct.
SWAP_LR = {f"TheSingingpumpkins/{f}" for f in (
    "DAY O resized.mp4", "Devil Went Down to Georgia Resized.mp4", "I Put a Spell on You resized.mp4",
    "Nightmare on My Street resized.mp4", "somebody watching me.mp4", "Spooky scary skeletons.mp4",
    "The Singing Pumpkin - Resized.mp4")}
NO_FIT = {"test"}   # categories staged untouched (projection grids)


def source(it):
    """Absolute input file for an item (joins multi-part raw sources into CACHE_DIR once)."""
    if it.get("raw") is None:
        return os.path.join(SRC, it["src"])
    raw = it["raw"]
    if isinstance(raw, str):
        return os.path.join(RAW, raw)
    parts = [os.path.join(RAW, r) for r in raw]
    dst = os.path.join(CACHE_DIR, it["id"] + ".mp4")
    if not (os.path.exists(dst) and os.path.getmtime(dst) >= max(map(os.path.getmtime, parts))):
        os.makedirs(CACHE_DIR, exist_ok=True)
        lst = dst + ".txt"
        with open(lst, "w") as f:
            f.writelines("file '%s'\n" % p.replace("'", "'\\''") for p in parts)
        subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", lst,
                        "-c", "copy", "-movflags", "+faststart", dst], check=True)
        os.remove(lst)
    return dst


def staged_tag(path):
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format_tags=comment",
                        "-of", "default=nw=1:nk=1", path], capture_output=True, text=True)
    return r.stdout.strip()


def slug(s):
    s = re.sub(r"\.mp4$", "", s, flags=re.I)
    s = re.sub(r"\b(re)?sized\b|\bresize\b|\bhd\b", "", s, flags=re.I)
    s = re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")
    return s[:60]


def probe_dur(path):
    return float(subprocess.check_output(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path]))


def measure_lufs(path):
    r = subprocess.run(["ffmpeg", "-nostats", "-hide_banner", "-vn", "-i", path,
                        "-af", "ebur128", "-f", "null", "-"], capture_output=True, text=True)
    m = re.findall(r"I:\s+(-?[\d.]+) LUFS", r.stderr)
    return float(m[-1]) if m else -70.0


def classify(rel):
    """-> (family, category, title, genre) or None to skip."""
    base = os.path.basename(rel)[:-4]
    if rel in BRIDGES or rel == ATMOS_TRIO_BUFFER:
        return None  # handled separately
    if rel.startswith("Custom Suno Pumpkins - Composited/"):
        title, _, genre = base.partition(" - ")
        return "SUNO", "suno", " ".join(title.split()), " ".join(genre.split()) or None
    if rel.startswith("TheSingingpumpkins/"):
        if "Transition Files" in rel:
            return None
        t = re.sub(r"\s*(-\s*)?\b(re)?sized?\b|\s*-\s*resize\b|\bHD\b", "", base, flags=re.I)
        return "TSP", "tsp", " ".join(t.split()), None
    if rel.startswith("Atmosfx Pumpkins/"):
        if "Extra Buffer Files" in rel:
            return None
        t = re.sub(r"^JOLJ\d_|_Trio_Pumpkin$| Trio$", "", base)
        t = re.sub(r"^Jack O.? Lantern Jamboree - Hologram - Faces Only - Pumpkin Trio - (Song|Story) - ", "", t)
        cat = ("atmos_scare" if "/Pumpkin Scares/" in rel else
               "atmos_tale" if "/Tales And Treats/" in rel else "atmos_song")
        return "ATMOS", cat, t.strip(" _"), None
    if rel.startswith("Trick or treaters/Opening/"):
        return "ATMOS", "tot_open", base, None
    if rel.startswith("Trick or treaters/Close out /"):
        return "ATMOS", "tot_close", base, None
    if rel.startswith("Calibration/"):
        return "TEST", "test", base, None  # make_test_grid.py: projection grid, never in DJ/show
    if rel.startswith("Trick or treaters/Clips 2.0/"):
        return "ATMOS", "tot_clip", base, rel.split("/")[2]  # genre field = source show
    return None


def scan():
    items = []
    for root, _, files in os.walk(SRC):
        for f in sorted(files):
            if not f.lower().endswith(".mp4"):
                continue
            rel = os.path.relpath(os.path.join(root, f), SRC)
            if rel in BRIDGES:
                fam, role = BRIDGES[rel]
                items.append(dict(src=rel, family=fam, category="buffer",
                                  title=os.path.basename(rel)[:-4], genre=None))
            elif rel == ATMOS_TRIO_BUFFER:
                items.append(dict(src=rel, family="ATMOS", category="buffer",
                                  title="JOLJ3 Buffer Trio", genre=None))
            else:
                c = classify(rel)
                if c:
                    fam, cat, title, genre = c
                    items.append(dict(src=rel, family=fam, category=cat, title=title, genre=genre,
                                      raw=TSP_RAW.get(rel)))
    for title, raw in EXTRA_TSP.items():
        items.append(dict(src="RAW/" + raw, family="TSP", category="tsp", title=title, genre=None, raw=raw))
    # stable order: family, category, title
    order = {"suno": 0, "tsp": 1, "atmos_song": 2, "atmos_tale": 3, "atmos_scare": 4,
             "tot_open": 5, "tot_clip": 6, "tot_close": 7}
    items.sort(key=lambda i: (order.get(i["category"], 9), i["category"], i["title"].lower()))
    seen = set()
    for n, it in enumerate(items, 1):
        s = slug(it["title"] if not it["category"].startswith("tot") else os.path.basename(it["src"]))
        while s in seen:
            s += "_b"
        seen.add(s)
        it["id"] = s
        it["n"] = n  # jukebox number (CMD_JUKEBOX_SELECT payload_a); stable while the folder is unchanged
        it["file"] = f"media/{it['family'].lower()}/{s}.mp4"
        it["label"] = make_label(it)
    return items


GROUP_NAME = {"test": "Test", "suno": "Suno", "tsp": "TSP", "atmos_song": "Atmos song", "atmos_tale": "Atmos tale",
              "atmos_scare": "Atmos scare", "tot_open": "ToT opening", "tot_clip": "ToT clip",
              "tot_close": "ToT close out"}


def make_label(it):
    """Plain-ASCII picker label for Home Assistant: '12 - Suno - Blood in the Corn (Slow Folk)'."""
    if it["category"] not in GROUP_NAME:
        return None
    title = it["title"]
    if it["category"].startswith("tot"):
        title = re.sub(r"^.*?_\d{2}_", "", title.lower()).replace("_", " ")
    if it["category"] == "suno" and it["genre"]:
        title += f" ({it['genre']})"
    label = f"{it['n']} - {GROUP_NAME[it['category']]} - {title}"
    label = label.encode("ascii", "ignore").decode()
    return label[:72]


def stage(it):
    src = source(it)
    dst = os.path.join(OUT, it["file"])
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    fit = it["category"] not in NO_FIT
    if fit:
        assigned = face_fit.assign(face_fit.detect(src, FACE_CACHE), it["src"] in SWAP_LR)
        tag = face_fit.fit_tag(assigned, src)
    if (os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src)
            and (not fit or staged_tag(dst) == tag)):
        it["dur"] = round(probe_dur(dst), 3)
        return it, "skip"
    src_lufs = measure_lufs(src)
    dur = probe_dur(src)
    af = []
    if src_lufs > SILENT_BELOW_LUFS:
        af.append(f"loudnorm=I={TARGET_LUFS}:TP=-1.5:LRA=11")
    af += [f"afade=t=in:d={AFADE}", f"afade=t=out:st={max(0, dur - AFADE):.3f}:d={AFADE}"]
    tmp = dst + ".part.mp4"
    if not fit:
        ins, vid = [], ["-map", "0:v:0", "-c:v", "copy"]
    else:
        ins = ["-i", face_fit.MASK_A]
        vid = ["-filter_complex", face_fit.fit_filter(assigned), "-map", "[v]", "-c:v", "libx264",
               "-preset", "fast", "-crf", "17", "-profile:v", "high", "-level", "4.1", "-metadata", f"comment={tag}"]
    r = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", src, *ins, *vid, "-map", "0:a:0?",
                        "-t", f"{dur:.3f}", "-af", ",".join(af), "-ar", "48000", "-ac", "2",
                        "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", tmp],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"ffmpeg failed on {it['src']}: {r.stderr[-400:]}")
    # Guard (2026-10-02 night: a looped mask input once ran the video 6 min past the song):
    # every stream of the staged file must end within 0.5 s of the source, or the clip fails loudly.
    got = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_type,duration",
                          "-of", "csv=p=0", tmp], capture_output=True, text=True).stdout.split()
    bad = [g for g in got if g.count(",") == 1 and g.split(",")[1] not in ("", "N/A")
           and abs(float(g.split(",")[1]) - dur) > 0.5]
    if bad:
        os.remove(tmp)
        raise RuntimeError(f"length check failed on {it['src']}: source {dur:.2f}s, staged {bad}")
    os.replace(tmp, dst)
    it["src_lufs"] = src_lufs
    it["dur"] = round(probe_dur(dst), 3)
    return it, "staged"


def preview(items):
    """One fitted frame per clip (ovals drawn in) -> contact sheets in PREVIEW_DIR. For checking."""
    from PIL import Image, ImageDraw
    os.makedirs(PREVIEW_DIR, exist_ok=True)

    def one(it):
        src = source(it)
        assigned = face_fit.assign(face_fit.detect(src, FACE_CACHE), it["src"] in SWAP_LR)
        t = min(60.0, probe_dur(src) / 2)
        png = os.path.join(CACHE_DIR, "preview", f"{it['n']:03d}.png")
        os.makedirs(os.path.dirname(png), exist_ok=True)
        subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-ss", f"{t:.1f}", "-i", src,
                        "-i", face_fit.MASK_A, "-filter_complex", face_fit.fit_filter(assigned),
                        "-map", "[v]", "-frames:v", "1", png], check=True)
        return it, png

    fits = [it for it in items if it["category"] not in NO_FIT]
    with ThreadPoolExecutor(4) as ex:
        shots = list(ex.map(one, fits))
    face_fit.save(FACE_CACHE)
    tw, th, per = 640, 360, 12
    for p in range(0, len(shots), per):
        sheet = Image.new("RGB", (2 * tw, (per // 2) * th), "white")
        d = ImageDraw.Draw(sheet)
        for i, (it, png) in enumerate(shots[p:p + per]):
            im = Image.open(png).convert("RGB")
            dd = ImageDraw.Draw(im)
            for (cx, cy), a, b, _ in face_fit.OVALS:
                dd.ellipse([cx - a, cy - b, cx + a, cy + b], outline=(0, 160, 0), width=3)
            x, y = (i % 2) * tw, (i // 2) * th
            sheet.paste(im.resize((tw - 4, th - 4)), (x, y))
            d.text((x + 6, y + 4), f"{it['n']} {it['family']} {it['title'][:50]}", fill=(255, 255, 255))
        out = os.path.join(PREVIEW_DIR, f"sheet_{p // per + 1:02d}.jpg")
        sheet.save(out, quality=88)
        print("wrote", out)


# ---- Face-group transitions (Joe 2026-09-27: "center fade") -------------------------
# One file per ordered pair of face groups: the old group's resting faces close into the
# centre of each pumpkin (iris), then the new group's resting faces open from the same
# centres. Played only when the face group changes; same-group songs play back to back.
# The iris is each pumpkin's own oval (face_fit.OVALS); fully open = the whole oval.
IRIS_FEATHER = 24    # soft edge, px
IRIS_HALF = 1.25     # seconds to close, and again to open
REST = {             # resting-face footage per group: (source, start s)
    "ATMOS": ("Trick or treaters/Buffer /flickering candle.mp4", 0.0),
    "TSP":   ("TheSingingpumpkins/TheSingingPumpkins_Transition Files/out fade.mp4", 0.0),
    "SUNO":  ("Custom Suno Pumpkins - Composited/Blood in the Corn.mp4", 0.5),
}


def _iris_geq(closing):
    x = f"clip(T/{IRIS_HALF},0,1)"
    e = f"(ld(0)*ld(0)*(3-2*ld(0)))"
    if closing:
        e = f"(1-{e})"
    fs = [f"clip(({1 - IRIS_FEATHER / (2 * min(a, b))}*ld(1)-hypot((X-{cx})/{a},(Y-{cy})/{b}))"
          f"*{min(a, b)}/{IRIS_FEATHER}+0.5,0,1)" for (cx, cy), a, b, _ in face_fit.OVALS]
    f = f"max(max({fs[0]},{fs[1]}),{fs[2]})"
    pre = f"st(0,{x});st(1,{e});"
    return ":".join(f"{c}='{pre}{c}(X,Y)*{f}'" for c in ("r", "g", "b"))


def make_transitions(force=False):
    out = []
    fams = list(REST)
    for a in fams:
        for b in fams:
            if a == b:
                continue
            rel = f"media/transitions/{a.lower()}_to_{b.lower()}.mp4"
            dst = os.path.join(OUT, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            (sa, ta), (sb, tb) = REST[a], REST[b]
            fits = [face_fit.assign(face_fit.detect(os.path.join(SRC, sx), FACE_CACHE)) for sx in (sa, sb)]
            tag = "fit=" + face_fit.fit_tag(fits, f"{a}>{b}/{IRIS_FEATHER}/{IRIS_HALF}")[4:]
            if force or not os.path.exists(dst) or staged_tag(dst) != tag:
                norm = "scale=1920:1080,fps=30,setpts=PTS-STARTPTS,format=gbrp"
                pre = ["[3:v]split[m0][m1]"]        # resting faces fitted like the songs
                for idx in (0, 1):
                    pre.append(face_fit.fit_filter(fits[idx], f"{idx}:v", f"m{idx}", f"k{idx}"))
                ins = ["k0", "k1"]
                fc = ";".join(pre + [f"[{ins[0]}]{norm},geq={_iris_geq(True)}[o]",
                                     f"[{ins[1]}]{norm},geq={_iris_geq(False)}[i]",
                                     "[o][i]concat=n=2:v=1:a=0,format=yuv420p[v]"])
                subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y",
                                "-ss", str(ta), "-t", str(IRIS_HALF), "-i", os.path.join(SRC, sa),
                                "-ss", str(tb), "-t", str(IRIS_HALF), "-i", os.path.join(SRC, sb),
                                "-f", "lavfi", "-t", str(2 * IRIS_HALF), "-i", "anullsrc=r=48000:cl=stereo",
                                "-i", face_fit.MASK_A,
                                "-filter_complex", fc, "-map", "[v]", "-map", "2:a", "-metadata", f"comment={tag}",
                                "-c:v", "libx264", "-crf", "17", "-r", "30", "-c:a", "aac", "-b:a", "128k",
                                "-shortest", "-movflags", "+faststart", dst], check=True)
            out.append(dict(id=f"transition_{a.lower()}_to_{b.lower()}", n=0, family=b, **{"from": a},
                            category="transition", title=f"{a} -> {b}", genre=a, src=None,
                            file=rel, dur=round(probe_dur(dst), 3)))
    return out


def fmt(sec):
    return f"{int(sec // 60)}:{sec % 60:04.1f}"


def write_outputs(items):
    lib = [{k: it.get(k) for k in ("n", "id", "family", "category", "title", "genre", "label", "from", "file", "dur")}
           for it in items]
    cues = skel_cues.update(lib)          # skeleton jaw moments, audio only (skel_cues.py)
    for it in lib:
        if cues.get(it["id"]):
            it["skel"] = cues[it["id"]]
    with open(os.path.join(OUT, "library.json"), "w") as f:
        json.dump({"version": 1, "items": lib}, f, indent=1)
    lines = ["# Singing Pumpkins — track list + durations", "",
             f"Generated by `build_card.py`. Audio normalized to {TARGET_LUFS} LUFS. `#` = jukebox number.", ""]
    cats = {}
    for it in items:
        cats.setdefault(it["category"], []).append(it)
    for cat, its in cats.items():
        tot = sum(i["dur"] for i in its)
        lines += [f"## {cat} — {len(its)} clips, {fmt(tot)} total", "",
                  "| # | title | genre / show | duration | card file |", "|---|---|---|---|---|"]
        lines += [f"| {i['n'] or ''} | {i['title']} | {i['genre'] or ''} | {fmt(i['dur'])} | `{i['file']}` |"
                  for i in its]
        lines.append("")
    md = "\n".join(lines)
    with open(os.path.join(OUT, "TRACKS.md"), "w") as f:
        f.write(md)
    with open(os.path.join(HERE, "TRACKS.md"), "w") as f:  # repo copy
        f.write(md)
    for name in ("autorun.brs", "show.json"):
        shutil.copy2(os.path.join(HERE, name), os.path.join(OUT, name))


FACE_CACHE = face_fit._load()


def main():
    items = scan()
    face_fit.make_mask()
    if "--preview" in sys.argv:
        preview(items)
        return
    if "--dry-run" in sys.argv:
        for it in items:
            print(f"{it['n']:3d} {it['family']:5s} {it['category']:12s} {it['file']}")
        print(len(items), "items")
        return
    os.makedirs(OUT, exist_ok=True)
    done = 0
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 4
    with ThreadPoolExecutor(jobs) as ex:
        for it, what in ex.map(stage, items):
            done += 1
            print(f"[{done}/{len(items)}] {what:6s} {it['file']}  {it['dur']:.1f}s", flush=True)
    face_fit.save(FACE_CACHE)
    items += make_transitions(force="--transitions" in sys.argv)
    face_fit.save(FACE_CACHE)
    for role in ("in", "out"):  # retired generated Suno bridges (pre iris transitions)
        old = os.path.join(OUT, f"media/suno/_bridge_{role}.mp4")
        if os.path.exists(old):
            os.remove(old)
    write_outputs(items)
    print("card ready:", OUT)


if __name__ == "__main__":
    main()
