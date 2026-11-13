"""middle_splice.py -- runtime assembly of a live MIDDLE line for the
template+insert architecture (2026-09-23, PROJECT_LOG.md same date).

The pre-rendered PART_A / PART_B fragments for every (character, bucket,
line index) come from `voice/scripts/render_middle_templates.py`, which
split each `gen_middle_templates.py` line on its single {CLOTHING}
placeholder and wrote a per-bucket MANIFEST.json. At showtime this module:

  1. looks up the chosen line's A/B fragment files in that manifest
  2. synthesizes ONLY the {CLOTHING} text live, via live_tts._synth_one
     (the one real-time cost in this design -- ~10-19s measured, vs.
     29-243s for a full live line)
  3. concatenates A + insert + B into one seamless raw line (fragments are
     format-normalized first -- the character FX chains, run externally
     per xtts_clone.py's Preset.fx_script, aren't guaranteed to share one
     sample rate/channel layout)
  4. runs the SAME loudnorm+denoise+ambient-bed "show" pass used on every
     other pre-rendered pool line (treat_pools.sh's treat_one(), inlined
     here) ONCE on the whole assembled line -- never on the isolated
     fragments, see render_middle_templates.py's docstring for why.

Never raises -- returns False on any failure so the caller falls back to
the static MiddleBank line, same fail-safe contract as the rest of
middle_audio.py.
"""
from __future__ import annotations

import json
import os
import random
import subprocess
import sys
from collections import deque
from pathlib import Path

HERE = Path(__file__).resolve().parent
ORCH = HERE.parent
sys.path.insert(0, str(ORCH))

from livebridge import live_tts, slot_fit  # noqa: E402

POOLS = ORCH / "voice" / "out" / "pools"
BEDS = ORCH / "voice" / "beds"

LOUDNORM = "loudnorm=I=-18:TP=-1.5:LRA=11"
DENOISE = "afftdn=nr=12:tn=1"


# Pause between the headcount sentence and the template line. The template
# fragments themselves are joined with NO gap (one continuous sentence); the
# headcount is its own sentence, so it gets a short natural beat.
HEADCOUNT_GAP_S = 0.35
ANTI_REPEAT_LAST_N = 10   # don't reuse one of a bucket's last N template indices
_recent: dict[str, deque] = {}


def _manifest(character: str, bucket: str) -> dict:
    p = POOLS / character / "middle_templates" / bucket / "MANIFEST.json"
    return json.loads(p.read_text())


def pick_template(bucket: str, insert: str, lead: str = "scarecrow",
                  partner: str | None = "burt") -> tuple[int, str, str | None] | None:
    """Choose a template for this visit's insert. Returns (index, fitted text
    for `lead`, fitted text for `partner` or None), or None.

    Fixed rules (slot_fit.py): only rendered entries whose slot the insert
    genuinely fits (after adjustment) are eligible. Indices where the matched
    partner line (Burt's reaction to Scarecrow's line N) is also rendered and
    fits are preferred, so both characters get a correct line. Last
    ANTI_REPEAT_LAST_N picks avoided. None if nothing fits / nothing rendered.
    """
    try:
        lead_m = _manifest(lead, bucket)
    except Exception:
        return None
    try:
        part_m = _manifest(partner, bucket) if partner else {}
    except Exception:
        part_m = {}

    def fitted(entry: dict | None) -> str | None:
        if not entry or not entry.get("done"):
            return None
        a, b = entry.get("a_text", ""), entry.get("b_text", "")
        return slot_fit.fit(insert, slot_fit.slot_class(a, b), a)

    both, lead_only = [], []
    for key, entry in lead_m.items():
        lt = fitted(entry)
        if lt is None:
            continue
        pt = fitted(part_m.get(key))
        (both if pt is not None else lead_only).append((int(key), lt, pt))
    pool = both or lead_only
    if not pool:
        return None
    recent = _recent.setdefault(f"{lead}/{bucket}", deque(maxlen=ANTI_REPEAT_LAST_N))
    fresh = [c for c in pool if c[0] not in recent] or pool
    choice = random.choice(fresh)
    recent.append(choice[0])
    return choice


def headcount_clip(key: str | None) -> Path | None:
    """Pre-rendered Scarecrow headcount lead-in for a key from
    middle_pipeline._headcount_key() ('a2_k3', 'large'), or None. Scarecrow
    only by design (docs/CURRENT-BUILD.md section 2)."""
    if not key:
        return None
    d = POOLS / "scarecrow" / "headcount"
    for name in (f"{key}_fx.wav", f"{key}.wav"):
        if (d / name).exists():
            return d / name
    return None


def _silence(seconds: float, out_path: Path) -> Path:
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i",
                    "anullsrc=r=24000:cl=mono", "-t", f"{seconds:.3f}",
                    "-c:a", "pcm_s16le", str(out_path)],
                   check=True, capture_output=True)
    return out_path


def _concat_seamless(files: list[Path], out_path: Path) -> None:
    """Normalize each fragment to a common format then concatenate with NO
    gap -- these are fragments of one continuous sentence, unlike
    xtts_clone.py's `_stitch` (which always inserts a pause -- it builds a
    demo reel of independent lines, not a spliced single line)."""
    tmp = out_path.parent / f"_concat_tmp_{out_path.stem}"
    tmp.mkdir(parents=True, exist_ok=True)
    norm = []
    for i, f in enumerate(files):
        n = tmp / f"{i:03d}.wav"
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", str(f),
                        "-ac", "1", "-ar", "24000", str(n)],
                       check=True, capture_output=True)
        norm.append(n)
    inputs = []
    for n in norm:
        inputs += ["-i", str(n)]
    filt = ("".join(f"[{i}:a]" for i in range(len(norm)))
            + f"concat=n={len(norm)}:v=0:a=1[out]")
    subprocess.run(["ffmpeg", "-y", "-v", "error", *inputs, "-filter_complex", filt,
                    "-map", "[out]", "-c:a", "pcm_s16le", str(out_path)],
                   check=True, capture_output=True)
    for n in tmp.iterdir():
        n.unlink()
    tmp.rmdir()


def _show_treatment(raw_path: Path, character: str, out_path: Path) -> bool:
    """Same ffmpeg chain as treat_pools.sh's treat_one(), applied once to
    the fully-assembled line. Returns False (never raises) on any failure."""
    bed = BEDS / f"bed_{character}.wav"
    if not bed.exists():
        return False
    try:
        dur = float(subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "csv=p=0", str(raw_path)],
            check=True, capture_output=True, text=True).stdout.strip())
        fo = max(dur - 0.3, 0.0)
        bl = dur + 0.3
        filt = (f"[0:a]{LOUDNORM},aresample=24000,aformat=channel_layouts=mono,"
                f"{DENOISE},apad=pad_dur=0.2[v];"
                f"[1:a]atrim=0:{bl},afade=t=in:d=0.25,afade=t=out:st={fo}:d=0.3[w];"
                f"[v][w]amix=inputs=2:duration=first:normalize=0,alimiter=limit=0.97")
        subprocess.run(
            ["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(raw_path),
             "-stream_loop", "-1", "-i", str(bed), "-filter_complex", filt,
             "-ar", "24000", "-ac", "1", "-sample_fmt", "s16", str(out_path)],
            check=True, capture_output=True)
        return out_path.exists()
    except Exception:
        return False


def splice_middle_line(character: str, bucket: str, index: int, clothing_text: str,
                       out_path: str | Path, log=print,
                       headcount_key: str | None = None) -> bool:
    """Assemble one live MIDDLE line: [pre-rendered headcount clip, Scarecrow
    only, optional] + pre-rendered PART_A + live {CLOTHING} insert +
    pre-rendered PART_B, then the show bed/loudnorm pass.

    `out_path` appears atomically (written under a temp name, then renamed),
    so the firmware's existence check can never fetch a half-written file.

    `index` is 1-based, matching MANIFEST.json's keys and the source
    .txt's line order. Returns True and writes `out_path` on success;
    returns False (never raises) on any failure -- caller falls back to
    MiddleBank."""
    out_path = Path(out_path)
    try:
        manifest = _manifest(character, bucket)
    except Exception as e:
        log(f"[middle_splice] no manifest for {character}/{bucket}: {e!r}")
        return False

    entry = manifest.get(f"{index:02d}")
    if not entry:
        log(f"[middle_splice] no entry {index:02d} in {character}/{bucket}")
        return False

    pool_dir = POOLS / character / "middle_templates" / bucket
    a_file = entry.get("a_file")
    b_file = entry.get("b_file")

    insert_path = out_path.with_name(out_path.stem + "_insert.wav")
    if not live_tts._synth_one(clothing_text, character, insert_path, log):
        log(f"[middle_splice] live insert synthesis failed: {clothing_text!r}")
        return False

    parts = []
    hc = headcount_clip(headcount_key) if character == "scarecrow" else None
    if headcount_key and character == "scarecrow" and hc is None:
        log(f"[middle_splice] headcount clip {headcount_key!r} not rendered -- skipping lead-in")
    if hc is not None:
        parts.append(hc)
        parts.append(_silence(HEADCOUNT_GAP_S,
                              out_path.with_name(out_path.stem + "_gap.wav")))
    if a_file:
        parts.append(pool_dir / a_file)
    parts.append(insert_path)
    if b_file:
        parts.append(pool_dir / b_file)

    raw_path = out_path.with_name(out_path.stem + "_assembled_raw.wav")
    try:
        _concat_seamless(parts, raw_path)
    except Exception as e:
        log(f"[middle_splice] concat failed: {e!r}")
        return False

    staged = out_path.with_name(out_path.stem + "_staged.wav")
    try:
        if not _show_treatment(raw_path, character, staged):
            log("[middle_splice] show treatment failed, using unbedded assembled line")
            raw_path.replace(staged)
        if not (staged.exists() and staged.stat().st_size > 44):
            log("[middle_splice] assembled file missing/empty")
            return False
        os.replace(staged, out_path)          # atomic: firmware never sees a partial file
        return True
    except Exception as e:
        log(f"[middle_splice] finalize failed: {e!r}")
        return False
    finally:
        for tmp in (insert_path, raw_path, staged,
                    out_path.with_name(out_path.stem + "_gap.wav")):
            try:
                tmp.unlink(missing_ok=True)
            except Exception:
                pass
