"""live_tts.py -- live, per-visitor XTTS synthesis of the OpenAI-generated
`villain_line`.

Kept warm as a module-level singleton: XTTS-v2 takes ~53s to load on this
CPU-only box, so `warm_up()` is meant to be called once at bridge startup
(from `middle_audio.py`'s `ensure_loaded()`, which `service.py` already
calls on start) so that cost never lands on a real visitor. Per-line
synthesis itself still takes real time -- benchmarked 2026-09-17 at ~3.8x
realtime (a ~5s line costs ~19-20s of wall clock).

History: this exact approach (live XTTS per visitor) was tried and pulled
back out on 2026-09-11 because the firmware's MIDDLE poll window was only
4 seconds -- live synthesis could never land in time, "no matter how it's
scheduled" (see middle_audio.py's module docstring). It's back 2026-09-17
because Joe, with the prop actually deployed and real field data in hand,
decided he'd rather wait up to ~90s than get nothing -- which requires the
firmware side to also move off a single blocking poll onto the HOLD loop
(tracked separately, not yet built as of this file's creation).
"""
from __future__ import annotations

import re
import sys
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
ORCH = HERE.parent
sys.path.insert(0, str(ORCH))

_lock = threading.Lock()
_cloner = None   # lazy XttsCloner singleton, set by warm_up()


def warm_up(log=print) -> bool:
    """Load the XTTS-v2 model now, synchronously, if not already loaded.
    Idempotent -- safe to call from every request path as a cheap no-op
    once warm. Returns False (never raises) if the model can't be loaded,
    so callers can treat live synthesis as simply unavailable."""
    global _cloner
    with _lock:
        if _cloner is not None:
            return True
        try:
            from voice.xtts_clone import XttsCloner
            _cloner = XttsCloner(verbose=False)
            log("[live_tts] XTTS-v2 model loaded and warm")
            return True
        except Exception as e:
            log(f"[live_tts] model load failed: {e!r} -- live synthesis unavailable")
            return False


def _synth_one(text: str, character: str, out_path: Path, log) -> bool:
    """Synthesize one chunk of text straight to out_path. Never raises --
    returns False on any failure, including "produced a suspiciously
    tiny/empty file". Blocking and slow by design -- callers must run this
    off any latency-sensitive thread."""
    if not warm_up(log=log):
        return False
    try:
        with _lock:   # XttsCloner isn't documented thread-safe; serialize
            raw_path = _cloner.say(text, speaker_wav=None, out_path=out_path,
                                   preset=character, fx=True)
        fx_path = raw_path.with_name(raw_path.stem + "_fx.wav")
        final = fx_path if fx_path.exists() else raw_path
        if final != out_path:
            final.replace(out_path)
        return out_path.exists() and out_path.stat().st_size > 44
    except Exception as e:
        log(f"[live_tts] synthesis failed for {character!r}: {e!r}")
        return False


def synthesize_line(text: str, character: str, out_path: str | Path,
                    log=print) -> bool:
    """Synthesize `text` (as ONE chunk) in `character`'s locked voice preset
    ("scarecrow" or "burt") straight to out_path (24kHz, matching the rest
    of the show's audio format). Applies the character's FX chain
    (pitch/EQ/echo -- same as the pre-rendered pools) when available.
    Never raises -- returns False on any failure so the caller can fall
    back to the pre-rendered bank cleanly."""
    text = text.strip()
    if not text:
        return False
    return _synth_one(text, character, Path(out_path), log)


_SENTENCE_SPLIT = re.compile(r"(?<=[.!?])\s+")


def split_sentences(text: str) -> list[str]:
    """Split into sentence-ish chunks for pipelined synthesis. Deliberately
    simple (no abbreviation/ellipsis handling) -- villain_line is already a
    short, model-generated line, not arbitrary prose."""
    return [s.strip() for s in _SENTENCE_SPLIT.split(text.strip()) if s.strip()]


def synthesize_line_parts(text: str, character: str, out_dir: str | Path,
                          session_id: int, log=print) -> int:
    """Pipelined synthesis: split `text` into sentences and synthesize them
    ONE AT A TIME, writing each part to disk (`{session_id:08x}_sc_partN.wav`,
    N starting at 1) as soon as it's ready -- part 1 is a real, complete file
    on disk well before part 2's synthesis even starts. This doesn't reduce
    total compute (XTTS-v2 time scales with output length either way), but
    it lets a poller (Scarecrow) start playing part 1 while part 2+ are
    still being generated, instead of waiting for the whole line to render
    before any audio exists at all -- Joe's "layering" idea, 2026-09-17.

    Stops at the first sentence that fails to synthesize (doesn't invent
    silence in the middle of a line) and returns the count of parts
    successfully written -- 0 means "no live line at all, use the bank",
    same fail-safe contract as synthesize_line(). The firmware-side poll
    loop to actually consume a variable number of parts is not built yet
    (see PROJECT_LOG.md 2026-09-17 night) -- this function is ready for it."""
    out_dir = Path(out_dir)
    sentences = split_sentences(text)
    if not sentences:
        return 0
    out_dir.mkdir(parents=True, exist_ok=True)
    done = 0
    for i, sentence in enumerate(sentences, start=1):
        part_path = out_dir / f"{session_id:08x}_sc_part{i}.wav"
        if not _synth_one(sentence, character, part_path, log):
            log(f"[live_tts] part {i}/{len(sentences)} failed — stopping "
                f"(serving {done} part(s) already on disk)")
            break
        done = i
        log(f"[live_tts] part {i}/{len(sentences)} ready: {sentence!r}")
    return done
