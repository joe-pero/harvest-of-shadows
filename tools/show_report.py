#!/usr/bin/env python3
"""Show-improvement report from the live bridge's event log (Joe 2026-09-28:
"we should also be collecting data for show improvement").

Read-only. Source: orchestration/livebridge/logs/show_events.jsonl (every
visit, cue, encore, zone entry, missed cue, mode change).

    python3 tools/show_report.py                 # today
    python3 tools/show_report.py 2026-09-28      # one day
    python3 tools/show_report.py all             # everything logged
"""
import json
import sys
from collections import Counter, defaultdict
from datetime import date
from pathlib import Path

LOG = Path(__file__).resolve().parent.parent / "orchestration/livebridge/logs/show_events.jsonl"


def main() -> int:
    day = sys.argv[1] if len(sys.argv) > 1 else date.today().isoformat()
    rows = []
    for line in LOG.open():
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if day == "all" or d.get("iso", "").startswith(day):
            rows.append(d)
    if not rows:
        print(f"no events for {day} in {LOG}")
        return 1

    ev = Counter(d["event"] for d in rows)
    summaries = [d for d in rows if d["event"] == "cycle_summary"]
    cues = [d for d in rows if d["event"] == "cue"]
    zones = Counter(d.get("zone") for d in rows if d["event"] == "zone_enter")
    by_hour = Counter(d["iso"][11:13] for d in summaries)

    print(f"== Harvest of Shadows show report: {day}  ({rows[0]['iso'][11:19]} - {rows[-1]['iso'][11:19]})")
    print(f"\nVisits: {len(summaries)}")
    for k, v in Counter(s.get("outcome") for s in summaries).most_common():
        print(f"  {k:<18} {v}")
    src = Counter(s.get("mainshow_source") for s in summaries if s.get("outcome") == "completed")
    if src:
        print("  main show fired from: " + ", ".join(f"{k or 'unknown'}={v}" for k, v in src.items()))
    trig = Counter(s.get("trigger") for s in summaries)
    if any(trig):
        print("  visit started by:    " + ", ".join(f"{k or 'unknown'}={v}" for k, v in trig.items()))

    durs = sorted(s["duration_s"] for s in summaries if s.get("duration_s") is not None)
    if durs:
        print(f"  visit length (s): median {durs[len(durs)//2]:.0f}, max {durs[-1]:.0f}")
    holds = [s.get("hold_count", 0) for s in summaries]
    if holds:
        print(f"  hold taunts per visit: avg {sum(holds)/len(holds):.1f}, max {max(holds)}")

    phase = Counter(c["phase"] for c in cues)
    print("\nCues sent: " + ", ".join(f"{k}={v}" for k, v in sorted(phase.items())))
    enc = Counter(c.get("trigger") for c in cues if c["phase"] == "encore")
    if enc:
        print("  encores: " + ", ".join(f"{k}={v}" for k, v in enc.items()))
    themes = Counter(c.get("theme") for c in cues if c["phase"] == "mainshow")
    if themes:
        print("  main shows by theme: " + ", ".join(f"{k}={v}" for k, v in themes.most_common()))

    print(f"\nProblems: cues NOT played={ev['cue_not_played']}  watchdog resets={ev['watchdog_reset']}  "
          f"Scarecrow silence events={ev['leader_silence']}")
    for d in rows:
        if d["event"] in ("cue_not_played", "watchdog_reset"):
            print(f"  {d['iso'][11:19]} {d['event']} {d.get('phase', '')} session={d.get('session_id', 0):08x}")

    if zones:
        print("\nZone entries (where people actually go): "
              + ", ".join(f"{k}={v}" for k, v in zones.most_common()))
        per_obj = defaultdict(set)
        for d in rows:
            if d["event"] == "zone_enter":
                per_obj[d.get("obj")].add(d.get("zone"))
        reached = sum(1 for z in per_obj.values() if "brick_walkway" in z)
        waited = sum(1 for z in per_obj.values() if z & {"zone_one", "waiting_zone"})
        print(f"  tracked people: {len(per_obj)}, reached the waiting spot: {waited}, "
              f"reached the brick walkway: {reached}")
    if by_hour:
        print("\nVisits by hour: " + "  ".join(f"{h}:00={n}" for h, n in sorted(by_hour.items())))
    modes = [d for d in rows if d["event"] == "mode"]
    if modes:
        print("Mode changes: " + ", ".join(f"{d['iso'][11:16]} gate_closed={d['gate_closed']}" for d in modes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
