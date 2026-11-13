"""banter_select.py -- the bridge picks which OUTRO banter exchange plays
(docs/DESIGN-outro-banter-playback.md 3.1, approved by Joe 2026-09-24).

Firmware stays a dumb player: the bridge chooses a pool + exchange number and
sends it in the retained harvest/show/params JSON as
    "banter": {"pool": "scary", "idx": 37, "lines": 3}
Pools = the rendered folders under voice/out/pools/_outro_banter/:
    scary, friendly                  regular MAIN-SHOW exchanges
    encore_scary, encore_friendly    "still here?" set for a lingering group

Fixed rules:
  - pool = cycle tone; encores use encore_<tone> first, and fall back to the
    regular pool's 2-line exchanges if the encore set isn't rendered.
  - only exchanges whose MANIFEST entry is done (fully rendered) are eligible.
  - anti-repeat for the whole night: never reuse one of the last
    RECENT_FRACTION of a pool; persisted in state/banter_recent.json so a
    bridge restart doesn't forget (the file is written atomically).
  - nothing eligible -> None, logged loudly; firmware then plays its own
    OUTRO pool (no params "banter" field = fallback path). Never silent.
Locked by orchestration/test_banter_select.py.
"""
from __future__ import annotations

import json
import os
import random
from pathlib import Path

HERE = Path(__file__).resolve().parent
BANTER_ROOT = HERE.parent / "voice" / "out" / "pools" / "_outro_banter"
STATE_FILE = HERE / "state" / "banter_recent.json"
RECENT_FRACTION = 0.4        # of each pool: scary 140 -> 56, encore_scary 24 -> 9


class BanterSelector:
    def __init__(self, root: Path = BANTER_ROOT, state_file: Path = STATE_FILE,
                 log=print, loud=None) -> None:
        self._root = Path(root)
        self._state_file = Path(state_file)
        self._log = log
        self._loud = loud or (lambda m: log(f"!!! {m}"))
        self._recent: dict[str, list[int]] = self._load_state()

    # ── state ────────────────────────────────────────────────────────────────
    def _load_state(self) -> dict[str, list[int]]:
        try:
            data = json.loads(self._state_file.read_text())
            return {k: [int(i) for i in v] for k, v in data.items()}
        except FileNotFoundError:
            return {}
        except Exception as e:
            self._loud(f"banter recent-state unreadable ({e!r}) -- starting fresh")
            return {}

    def _save_state(self) -> None:
        try:
            self._state_file.parent.mkdir(parents=True, exist_ok=True)
            tmp = self._state_file.with_suffix(".tmp")
            tmp.write_text(json.dumps(self._recent))
            os.replace(tmp, self._state_file)
        except Exception as e:
            self._loud(f"banter recent-state not saved ({e!r}) -- repeats possible after a restart")

    # ── pools ────────────────────────────────────────────────────────────────
    def _exchanges(self, pool: str) -> dict[int, int]:
        """{exchange idx: line count} for fully rendered exchanges."""
        try:
            m = json.loads((self._root / pool / "MANIFEST.json").read_text())
        except Exception:
            return {}
        out = {}
        for k, v in m.items():
            if v.get("done") and v.get("lines"):
                out[int(k)] = len(v["lines"])
        return out

    def _choose(self, pool: str, eligible: dict[int, int]) -> dict | None:
        if not eligible:
            return None
        keep = max(1, int(len(eligible) * RECENT_FRACTION))
        recent = self._recent.get(pool, [])[-keep:]
        fresh = [i for i in eligible if i not in recent] or list(eligible)
        idx = random.choice(fresh)
        self._recent[pool] = (self._recent.get(pool, []) + [idx])[-keep:]
        self._save_state()
        return {"pool": pool, "idx": idx, "lines": eligible[idx]}

    # ── public ───────────────────────────────────────────────────────────────
    def pick(self, tone: str, encore: bool = False) -> dict | None:
        """{"pool", "idx", "lines"} or None (firmware falls back to its OUTRO pool)."""
        tone = tone if tone in ("scary", "friendly") else "scary"
        if encore:
            choice = self._choose(f"encore_{tone}", self._exchanges(f"encore_{tone}"))
            if choice is None:
                two = {i: n for i, n in self._exchanges(tone).items() if n == 2}
                choice = self._choose(tone, two)
                if choice is not None:
                    self._log(f"[banter] encore set encore_{tone} not rendered -- "
                              f"using a 2-line {tone} exchange instead")
        else:
            choice = self._choose(tone, self._exchanges(tone))
        if choice is None:
            self._loud(f"no rendered banter for tone={tone} encore={encore} -- "
                       f"boards will play their SD OUTRO pool")
        return choice
