#!/usr/bin/env python3
"""Build the shoulder-crows loop track + its timing header from the 2025 card file.

Source: "Crow sounds.mp3" (2025 MP3-player card, ~3 h: ~15 distinct crow calls
repeated ~300 times between stretches of digital silence). We cut each distinct
call once and lay them out ourselves, so the call timings are known exactly.

Design (Joe, 2026-09-29): one fixed track for every show phase. The first call
starts at FIRST_CALL_MS, so a power-cycle of the player = a caw at a known time
(plus the player's boot delay, tuned on the bench). The rest of the calls are
30-60 s apart. The ESP32 power-cycles the player at the end of each loop to
stay in sync.

2026-10-01 (Joe: "longer crow sounds during active"): the first SHOW_BLOCK_S of the
track is a dense run of the LONG calls (short gaps, sometimes overlapping, like a
squabble). In show mode the ESP32 restarts the player at CROW_SHOW_BLOCK_MS, so the
show loops only that block; idle plays the whole loop (one burst at the top).

Usage: build_track.py <source.mp3> [outdir]
Writes: crows_loop.mp3 (for the player's card), calls/*.wav (extracted calls),
        ../src/crow_track.h (event table for the firmware), track_events.json
"""
import json, os, random, subprocess, sys
import numpy as np

SR = 44100
SCAN_SR = 8000           # call finding runs on 8 kHz mono (the full file at 44.1 k stereo = ~4 GB)
TRACK_S = 300.0          # 5 min loop
FIRST_CALL_MS = 500      # first call right after the player starts
GAP_S = (30.0, 60.0)     # idle part: call start to call start
SHOW_BLOCK_S = 40.0      # dense long-call block at the top (Joe: 40 s)
BLOCK_GAP_S = (-0.8, 2.5)  # block: end of one call to start of the next (<0 = overlap)
LONG_MIN_S = 2.4         # calls at least this long go in the block
SEED = 20260929          # fixed: the same track every build
PRE_S, POST_S = 0.05, 0.25  # padding around each cut call

def decode(path, sr, ch):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", str(ch), "-ar", str(sr),
                          "-f", "s16le", "-"], check=True, capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.int16).reshape(-1, ch).astype(np.float32) / 32768

def find_calls(mono, sr):
    hop = sr // 10                                   # 100 ms frames
    n = len(mono) // hop
    db = 20 * np.log10(np.sqrt((mono[: n * hop].reshape(n, hop) ** 2).mean(1)) + 1e-6)
    on = db > -60
    calls, i = [], 0
    while i < n:
        if on[i]:
            j = i
            while j < n and (on[j] or on[j : j + 6].any()):  # merge gaps < 0.6 s
                j += 1
            calls.append((i / 10, (j - i) / 10))
            i = j
        else:
            i += 1
    return calls

def family(mono, calls):
    """Group calls by duration + loudness envelope; return one representative each."""
    reps = []
    for t, d in calls:
        seg = mono[int(t * SCAN_SR) : int((t + d) * SCAN_SR)]
        k = max(1, len(seg) // 20)
        env = np.log(np.sqrt((seg[: k * 20].reshape(20, k) ** 2).mean(1)) + 1e-4)
        for r in reps:
            if abs(r["dur"] - d) <= 0.2 and np.corrcoef(env, r["env"])[0, 1] > 0.9:
                r["count"] += 1
                break
        else:
            reps.append({"t": t, "dur": d, "env": env, "count": 1})
    return reps

def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(__file__))
    os.makedirs(os.path.join(out, "calls"), exist_ok=True)
    mono = decode(src, SCAN_SR, 1)[:, 0]
    calls = find_calls(mono, SCAN_SR)
    reps = sorted(family(mono, calls), key=lambda r: -r["count"])
    del mono
    clips = []
    for i, r in enumerate(reps):
        # cut just this call from the source at full quality
        start = max(0.0, r["t"] - PRE_S)
        raw = subprocess.run(["ffmpeg", "-v", "error", "-ss", f"{start:.3f}", "-t", f"{r['dur'] + PRE_S + POST_S:.3f}",
                              "-i", src, "-ac", "2", "-ar", str(SR), "-f", "s16le", "-"],
                             check=True, capture_output=True).stdout
        clip = np.frombuffer(raw, dtype=np.int16).reshape(-1, 2).astype(np.float32) / 32768
        fade = int(0.02 * SR)
        clip[:fade] *= np.linspace(0, 1, fade)[:, None]
        clip[-fade:] *= np.linspace(1, 0, fade)[:, None]
        name = f"call_{i:02d}_{r['dur']:.1f}s"
        # audible onset inside the clip (first 10 ms frame above -50 dBFS): the event
        # table points at the caw itself, not at the padding/fade before it
        m = clip.mean(1)
        fr = len(m) // 441
        fdb = 20 * np.log10(np.sqrt((m[: fr * 441].reshape(fr, 441) ** 2).mean(1)) + 1e-6)
        onset_ms = int(np.argmax(fdb > -50)) * 10
        clips.append({"name": name, "audio": clip, "count": r["count"], "src_t": r["t"], "onset_ms": onset_ms})
        pcm = (np.clip(clip, -1, 1) * 32767).astype(np.int16).tobytes()
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", str(SR), "-ac", "2",
                        "-i", "-", os.path.join(out, "calls", name + ".wav")], input=pcm, check=True)

    # Layout: first call at FIRST_CALL_MS (a short, common caw), then 30-60 s apart,
    # cycling through the calls in a shuffled order without back-to-back repeats.
    rng = random.Random(SEED)
    short = [c for c in clips if len(c["audio"]) / SR <= 2.0]
    first = max(short, key=lambda c: c["count"])
    order = [c for c in clips if c is not first]
    rng.shuffle(order)
    events, t, k = [(FIRST_CALL_MS / 1000, first)], FIRST_CALL_MS / 1000, 0
    # Show block: long calls back to back after the first caw, no call cut off at the block end.
    longs = [c for c in clips if len(c["audio"]) / SR >= LONG_MIN_S]
    rng.shuffle(longs)
    end, j, prev = t + len(first["audio"]) / SR, 0, None
    while True:
        c = longs[j % len(longs)]
        j += 1
        if c is prev:
            continue
        s = max(t + 0.3, end + rng.uniform(*BLOCK_GAP_S))
        if s + len(c["audio"]) / SR > SHOW_BLOCK_S:
            break
        events.append((s, c))
        t, end, prev = s, max(end, s + len(c["audio"]) / SR), c
    block_end_ms = int(end * 1000) + 300
    t = max(t, SHOW_BLOCK_S - GAP_S[0])  # idle part: first sparse call 30-60 s after the block
    while True:
        t += rng.uniform(*GAP_S)
        c = order[k % len(order)]
        k += 1
        if t + len(c["audio"]) / SR > TRACK_S - 1.0:
            break
        events.append((t, c))

    track = np.zeros((int(TRACK_S * SR), 2), dtype=np.float32)
    for t, c in events:
        s = int(t * SR)
        track[s : s + len(c["audio"])] += c["audio"]
    pcm = (np.clip(track, -1, 1) * 32767).astype(np.int16).tobytes()
    mp3 = os.path.join(out, "crows_loop.mp3")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", str(SR), "-ac", "2", "-i", "-",
                    "-codec:a", "libmp3lame", "-b:a", "128k", mp3], input=pcm, check=True)

    ev = [{"start_ms": round(t * 1000) + c["onset_ms"],
           "dur_ms": round(len(c["audio"]) / SR * 1000) - c["onset_ms"], "call": c["name"]}
          for t, c in events]
    json.dump({"track_ms": int(TRACK_S * 1000), "show_block_ms": block_end_ms, "events": ev}, open(os.path.join(out, "track_events.json"), "w"),
              indent=1)
    hdr = os.path.join(out, "..", "src", "crow_track.h")
    os.makedirs(os.path.dirname(hdr), exist_ok=True)
    with open(hdr, "w") as f:
        f.write("// GENERATED by audio/build_track.py (seed %d) — do not edit by hand.\n" % SEED)
        f.write("// Audible call onsets in crows_loop.mp3, ms from the start of the track (player boot delay NOT included).\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("struct CrowCall { uint32_t startMs; uint16_t durMs; };\n")
        f.write("static const uint32_t CROW_TRACK_MS = %d;\n" % int(TRACK_S * 1000))
        f.write("static const uint32_t CROW_SHOW_BLOCK_MS = %d;  // show mode loops 0..this\n" % block_end_ms)
        f.write("static const CrowCall CROW_CALLS[] = {\n")
        for e in ev:
            f.write("  {%d, %d},  // %s\n" % (e["start_ms"], e["dur_ms"], e["call"]))
        f.write("};\nstatic const uint8_t CROW_CALL_COUNT = sizeof(CROW_CALLS) / sizeof(CROW_CALLS[0]);\n")
    print(f"{len(clips)} distinct calls, {len(events)} events in {TRACK_S:.0f} s -> {mp3}; show block 0-{block_end_ms} ms")
    for e in ev:
        print(f"  {e['start_ms']/1000:7.1f}s  {e['dur_ms']:5d} ms  {e['call']}")

if __name__ == "__main__":
    main()
