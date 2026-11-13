#!/usr/bin/env python3
"""Offline tests for livebridge/banter_select.py (no MQTT, no audio).

    python3 orchestration/test_banter_select.py
"""
from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from livebridge.banter_select import RECENT_FRACTION, BanterSelector  # noqa: E402


def _pool(root: Path, name: str, shapes: list[int], undone: set[int] = frozenset()) -> None:
    d = root / name
    d.mkdir(parents=True)
    m = {f"{i:03d}": {"done": i not in undone,
                      "lines": [{"speaker": "scarecrow"}] * n}
         for i, n in enumerate(shapes, 1)}
    (d / "MANIFEST.json").write_text(json.dumps(m))


def run() -> int:
    fails = 0

    def check(label, ok):
        nonlocal fails
        fails += 0 if ok else 1
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}")

    logs = []
    with tempfile.TemporaryDirectory() as t:
        root, state = Path(t) / "banter", Path(t) / "state" / "recent.json"
        _pool(root, "scary", [2, 3, 4, 2, 3] * 4, undone={5})          # 20, #5 not rendered
        _pool(root, "friendly", [3, 2, 3] * 3)
        _pool(root, "encore_scary", [2] * 6)

        sel = BanterSelector(root, state, log=logs.append, loud=logs.append)

        picks = [sel.pick("scary")["idx"] for _ in range(60)]
        check("never picks an unrendered exchange", 5 not in picks)
        keep = int(19 * RECENT_FRACTION)
        window_ok = all(picks[i] not in picks[max(0, i - keep):i] for i in range(len(picks)))
        check(f"no repeat within the last {keep} picks", window_ok)

        # restart: a new selector reads the persisted state
        last = picks[-keep:]
        sel2 = BanterSelector(root, state, log=logs.append, loud=logs.append)
        nxt = sel2.pick("scary")["idx"]
        check("anti-repeat survives a bridge restart", nxt not in last)

        e = sel2.pick("scary", encore=True)
        check("encore uses the encore pool", e["pool"] == "encore_scary" and e["lines"] == 2)

        f = sel2.pick("friendly", encore=True)                      # no encore_friendly rendered
        check("missing encore set -> a 2-line regular exchange",
              f["pool"] == "friendly" and f["lines"] == 2)
        check("...and it says so in the log", any("not rendered" in m for m in logs))

        check("unknown tone is treated as scary", sel2.pick("intense")["pool"] == "scary")

        empty = BanterSelector(Path(t) / "nothing", Path(t) / "s2.json",
                               log=logs.append, loud=logs.append)
        check("nothing rendered -> None (firmware falls back), logged loudly",
              empty.pick("scary") is None and any("no rendered banter" in m for m in logs))

        state.write_text("{corrupt")
        sel3 = BanterSelector(root, state, log=logs.append, loud=logs.append)
        check("corrupt state file -> starts fresh, logged, still picks",
              sel3.pick("scary") is not None and any("unreadable" in m for m in logs))

    print(f"\n{'ALL PASS' if fails == 0 else f'{fails} FAILURE(S)'}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(run())
