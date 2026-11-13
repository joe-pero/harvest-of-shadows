#!/usr/bin/env python3
"""Offline simulator of autorun.brs — same algorithm, run against the real library.

    python3 sim_card.py                 # 8 h of DJ mode + 200 SHOW plans, print stats
    python3 sim_card.py --lib PATH      # library.json (default: the card's, else a live scan)

Keep this in step with autorun.brs: PickFamily / Draw / StartShow mirror it 1:1.
Use it to tune show.json (dj_share, max_run, show_seconds) before touching the card.
"""
import json, os, random, subprocess, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
CARD_LIB = "/mnt/media/Pumpkin folder 2026/SD Card 2026/library.json"


def load_library():
    path = sys.argv[sys.argv.index("--lib") + 1] if "--lib" in sys.argv else CARD_LIB
    if os.path.exists(path):
        return json.load(open(path))["items"]
    sys.path.insert(0, HERE)
    import build_card  # fall back to scanning the sandbox + ffprobe
    items = build_card.scan()
    for it in items:
        it["dur"] = build_card.probe_dur(os.path.join(build_card.SRC, it["src"]))
    return items


class Card:
    def __init__(self, items, cfg):
        self.cfg = cfg
        self.pools, self.transitions = collections.defaultdict(list), {}
        for it in items:
            if it["category"] == "transition":
                self.transitions[(it["from"], it["family"])] = it
            elif it["category"] != "buffer":
                self.pools[it["category"]].append(it)
        self.decks, self.recent = {}, []
        self.cur_fam, self.run_len = "", 0
        self.played = {"SUNO": 0.0, "TSP": 0.0, "ATMOS": 0.0}

    def draw(self, cat):
        pool = self.pools.get(cat)
        if not pool:
            return None
        win = min(self.cfg["no_repeat_window"], len(pool) - 1)
        for _ in range(len(pool) * 2):
            if not self.decks.get(cat):
                self.decks[cat] = random.sample(pool, len(pool))
            it = self.decks[cat].pop()
            if it["id"] not in self.recent[len(self.recent) - win:] or win == 0:
                return it
        return random.choice(pool)

    def pick_family(self):
        total = sum(self.played.values())
        best, best_score = "", -1000
        for fam in ("SUNO", "TSP", "ATMOS"):
            if fam == self.cur_fam and self.run_len >= self.cfg["max_run"]:
                continue
            pct = self.played[fam] * 100 / total if total else 0
            score = self.cfg["dj_share"][fam] - pct + random.random() * 15 + (8 if fam == self.cur_fam else 0)
            if score > best_score:
                best, best_score = fam, score
        for fam in self.played:
            self.played[fam] *= 0.97
        return best or self.cur_fam

    def pick_atmos_cat(self):
        w = {k: v for k, v in self.cfg["atmos_mix"].items() if k in self.pools}
        return random.choices(list(w), weights=list(w.values()))[0]

    def dj_next(self):
        fam = self.pick_family()
        it = self.draw({"SUNO": "suno", "TSP": "tsp"}.get(fam) or self.pick_atmos_cat())
        seq = []
        if it["family"] != self.cur_fam:
            if self.cur_fam:
                seq.append(self.transitions[(self.cur_fam, it["family"])])
            self.cur_fam, self.run_len = it["family"], 0
        self.run_len += 1
        self.recent.append(it["id"])
        self.played[it["family"]] += it["dur"]
        return seq + [it]

    def plan_show(self):
        opening, closing = self.draw("tot_open"), self.draw("tot_close")
        remain = self.cfg["show_seconds"] - opening["dur"] - closing["dur"]
        clips, used, tries = [], set(), 0
        pool = self.pools["tot_clip"]
        while remain > 10 and tries < len(pool) * 2:
            c = self.draw("tot_clip")
            tries += 1
            if c and c["id"] not in used and c["dur"] <= remain:
                clips.append(c); used.add(c["id"]); remain -= c["dur"]
        return [opening] + clips + [closing]


def main():
    items = load_library()
    cfg = json.load(open(os.path.join(HERE, "show.json")))
    random.seed(1)
    card = Card(items, cfg)
    air, count, switches, bridge_sec, t = collections.Counter(), collections.Counter(), 0, 0.0, 0.0
    longest_gap_repeat, last_seen, n = 10**9, {}, 0
    while t < 8 * 3600:
        for it in card.dj_next():
            t += it["dur"]
            if it["category"] == "transition":
                bridge_sec += it["dur"]
                switches += 1
                continue
            n += 1
            air[it["family"]] += it["dur"]; count[it["category"]] += 1
            if it["id"] in last_seen:
                longest_gap_repeat = min(longest_gap_repeat, n - last_seen[it["id"]])
            last_seen[it["id"]] = n
    tot = sum(air.values())
    print(f"DJ, 8 h simulated ({n} songs/tales):")
    for fam in ("SUNO", "TSP", "ATMOS"):
        print(f"  {fam:5s} {air[fam] / tot * 100:5.1f}% of play time  (target {cfg['dj_share'][fam]}%)")
    print("  per category:", dict(count))
    print(f"  face-set changes: {switches} ({switches / 8:.1f}/h), transition time {bridge_sec / tot * 100:.1f}%")
    print(f"  closest repeat of any one song: {longest_gap_repeat} songs apart")
    lens = []
    for _ in range(200):
        plan = card.plan_show()
        lens.append((sum(i["dur"] for i in plan), len(plan) - 2))
    d = [l for l, _ in lens]
    print(f"SHOW plans (show_seconds={cfg['show_seconds']}): length {min(d):.0f}-{max(d):.0f}s, "
          f"avg {sum(d) / len(d):.0f}s, middle clips {min(c for _, c in lens)}-{max(c for _, c in lens)}")
    print("  example:", " -> ".join(f"{i['id'][:28]}({i['dur']:.0f}s)" for i in card.plan_show()))


if __name__ == "__main__":
    main()
