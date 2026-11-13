#!/usr/bin/env python3
"""Build the season charts in docs/charts/ from docs/charts/season_2026.json.

    python3 tools/make_charts.py

Every chart is drawn twice (light and dark) so it matches the reader's GitHub theme. Numbers
are aggregate only; nothing identifies a visitor. Colors are a validated categorical palette
(colorblind-safe in both modes); every value is labeled directly, so color never carries
meaning on its own.
"""
from __future__ import annotations

import json
import os
import statistics as st
from datetime import date

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import MaxNLocator  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "charts")
DATA = json.load(open(os.path.join(OUT, "season_2026.json")))

THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", muted="#898781", grid="#e1e0d9",
                  axis="#c3c2b7", s1="#2a78d6", s2="#eb6834", track="#e1e0d9"),
    "dark": dict(surface="#1a1a19", ink="#f0efec", ink2="#c3c2b7", muted="#898781", grid="#2c2c2a",
                 axis="#383835", s1="#3987e5", s2="#d95926", track="#2c2c2a"),
}
FONT = ["DejaVu Sans", "Liberation Sans", "sans-serif"]


def figure(t, w=8.0, h=3.6):
    fig, ax = plt.subplots(figsize=(w, h), dpi=150)
    fig.patch.set_facecolor(t["surface"])
    ax.set_facecolor(t["surface"])
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.spines["bottom"].set_color(t["axis"])
    ax.tick_params(colors=t["muted"], labelsize=9, length=0)
    ax.yaxis.grid(True, color=t["grid"], linewidth=0.8)
    ax.set_axisbelow(True)
    return fig, ax


def titles(fig, t, title, sub):
    fig.text(0.012, 0.965, title, color=t["ink"], fontsize=13, fontweight="bold", va="top", family=FONT)
    fig.text(0.012, 0.885, sub, color=t["ink2"], fontsize=9.5, va="top", family=FONT)


def save(fig, name, mode):
    fig.savefig(os.path.join(OUT, f"{name}-{mode}.png"), facecolor=fig.get_facecolor())
    plt.close(fig)


def label_bars(ax, t, xs, ys, fmt="{:,}", dy=0.6):
    for x, y in zip(xs, ys):
        ax.text(x, y + dy, fmt.format(y), ha="center", va="bottom", color=t["ink"], fontsize=9)


# ---------------------------------------------------------------------------------------------
def nights(t, mode):
    rows = DATA["nights"]
    labels = [date.fromisoformat(r["date"]).strftime("%b %-d") for r in rows]
    visits = [r["visits"] for r in rows]
    shows = [r["mainshow"] for r in rows]
    fig, ax = figure(t, 8.4, 3.9)
    x = range(len(rows))
    ax.bar([i - 0.17 for i in x], visits, width=0.32, color=t["s1"], label="Visits (group stopped)")
    ax.bar([i + 0.17 for i in x], shows, width=0.32, color=t["s2"], label="Main shows (walked up the path)")
    for i, (v, s) in enumerate(zip(visits, shows)):
        if v:
            ax.text(i - 0.17, v + 0.8, str(v), ha="center", va="bottom", color=t["ink"], fontsize=8.5)
            ax.text(i + 0.17, s + 0.8, str(s), ha="center", va="bottom", color=t["ink2"], fontsize=8.5)
    blind = [i for i, r in enumerate(rows) if r.get("note")]
    for i in blind:
        ax.text(i, 3, "camera\nblind", ha="center", va="bottom", color=t["muted"], fontsize=8, style="italic")
    ax.set_xticks(list(x), labels)
    ax.set_ylim(0, max(visits) * 1.18)
    ax.yaxis.set_major_locator(MaxNLocator(5, integer=True))
    leg = ax.legend(loc="upper left", frameon=False, fontsize=9, labelcolor=t["ink2"], handlelength=1, ncol=2)
    titles(fig, t, "Every show night of the 2026 season",
           f"{sum(visits)} visits and {sum(shows)} main shows over 10 nights, from the show's own event log")
    fig.subplots_adjust(left=0.06, right=0.99, top=0.76, bottom=0.11)
    save(fig, "season-nights", mode)


def halloween_hours(t, mode):
    h = DATA["halloween_by_hour"]
    fig, ax = figure(t, 6.0, 3.4)
    xs = list(range(len(h)))
    ys = list(h.values())
    ax.bar(xs, ys, width=0.42, color=t["s1"])
    label_bars(ax, t, xs, ys)
    ax.set_xticks(xs, [f"{k}–{int(k[:2]) + 1}:00" for k in h])
    ax.set_ylim(0, max(ys) * 1.2)
    ax.yaxis.set_major_locator(MaxNLocator(4, integer=True))
    titles(fig, t, "Halloween night, visits by hour", f"{sum(ys)} visits; half arrived in a single hour")
    fig.subplots_adjust(left=0.08, right=0.98, top=0.74, bottom=0.12)
    save(fig, "halloween-hours", mode)


def funnel(t, mode):
    steps = DATA["halloween_funnel"]
    fig, ax = figure(t, 8.0, 3.4)
    ax.yaxis.grid(False)
    ax.xaxis.grid(True, color=t["grid"], linewidth=0.8)
    ax.spines["bottom"].set_visible(False)
    names = [s[0] for s in steps][::-1]
    vals = [s[1] for s in steps][::-1]
    ys = range(len(steps))
    ax.barh(list(ys), vals, height=0.42, color=t["s1"])
    top = vals[-1]
    for y, v in zip(ys, vals):
        pct = "" if v == top else f"  ({v / top:.0%} of people tracked)"
        ax.text(v + top * 0.01, y, f"{v:,}{pct}", va="center", color=t["ink"], fontsize=9)
    ax.set_yticks(list(ys), names)
    ax.tick_params(axis="y", colors=t["ink2"], labelsize=9.5)
    ax.set_xlim(0, top * 1.45)
    ax.xaxis.set_major_locator(MaxNLocator(5, integer=True))
    titles(fig, t, "From the sidewalk to the show (Halloween night)",
           "Each step narrows; the AI is only called for groups that actually stop and stay")
    fig.subplots_adjust(left=0.30, right=0.98, top=0.76, bottom=0.08)
    save(fig, "halloween-funnel", mode)


def ai_cost(t, mode):
    a = DATA["ai"]
    cost, cap = a["season_cost_usd"], a["lifetime_cap_usd"]
    fig, ax = plt.subplots(figsize=(8.0, 1.9), dpi=150)
    fig.patch.set_facecolor(t["surface"])
    ax.set_facecolor(t["surface"])
    ax.axis("off")
    ax.barh([0], [cap], height=0.36, color=t["track"])
    ax.barh([0], [cost], height=0.36, color=t["s1"])
    ax.text(cost + cap * 0.012, 0, f"${cost:.2f} spent", va="center", color=t["ink"], fontsize=10, fontweight="bold")
    ax.text(cap, -0.42, f"${cap:.0f} hard cap (AI switches itself off here)", ha="right", va="top", color=t["ink2"], fontsize=9)
    ax.text(0, -0.42, "$0", ha="left", va="top", color=t["muted"], fontsize=9)
    ax.set_xlim(0, cap * 1.01)
    ax.set_ylim(-0.75, 0.45)
    calls = len(a["calls"])
    titles(fig, t, "Season AI spend against its hard cap",
           f"{calls} live AI calls, about {cost / calls * 100:.1f} cents each, {cost / cap:.0%} of the cap used")
    fig.subplots_adjust(left=0.02, right=0.98, top=0.62, bottom=0.05)
    save(fig, "ai-spend", mode)


def ai_latency(t, mode):
    secs = sorted(c["ms"] / 1000 for c in DATA["ai"]["calls"] if c["ms"])
    med = st.median(secs)
    fig, ax = figure(t, 8.0, 3.2)
    edges = list(range(int(min(secs)), int(max(secs)) + 2))
    counts = [sum(1 for s in secs if lo <= s < lo + 1) for lo in edges[:-1]]
    xs = [lo + 0.5 for lo in edges[:-1]]
    ax.bar(xs, counts, width=0.5, color=t["s1"])
    label_bars(ax, t, xs, counts, dy=0.15)
    ax.axvline(med, color=t["ink2"], linewidth=1, linestyle=(0, (3, 3)))
    ax.text(med - 0.12, max(counts) * 1.22, f"median {med:.1f} s", color=t["ink2"], fontsize=9, va="top", ha="right")
    ax.set_xticks(edges[::2], [f"{e} s" for e in edges[::2]])
    ax.set_ylim(0, max(counts) * 1.32)
    ax.yaxis.set_major_locator(MaxNLocator(4, integer=True))
    rejected = sum(1 for c in DATA["ai"]["calls"] if c["ok"] != "True")
    titles(fig, t, "How long the live AI took to answer",
           f"{len(secs)} live calls during the season; {rejected} line was rejected by the content screen")
    fig.subplots_adjust(left=0.06, right=0.98, top=0.74, bottom=0.12)
    save(fig, "ai-latency", mode)


def confidence(t, mode):
    c = DATA["detection_confidence_show_hours"]
    bins = {float(k): v for k, v in c["bins"].items()}
    xs = sorted(bins)
    ys = [bins[x] for x in xs]
    total = sum(ys)
    ordered = [x for x in xs for _ in range(bins[x])]
    med = st.median(ordered)
    fig, ax = figure(t, 8.0, 3.3)
    ax.bar(xs, ys, width=0.006, color=t["s1"])
    ax.axvline(0.70, color=t["ink2"], linewidth=1, linestyle=(0, (3, 3)))
    ax.text(0.702, max(ys) * 1.36, "70% threshold:\nbelow this, ignored", color=t["ink2"], fontsize=8.5, va="top")
    ax.axvline(med, color=t["ink"], linewidth=1)
    ax.text(med + 0.003, max(ys) * 1.36, f"median {med:.0%}", color=t["ink"], fontsize=9, va="top")
    ax.set_ylim(0, max(ys) * 1.4)
    ax.set_xlim(0.68, max(xs) + 0.02)
    ax.set_xticks([0.70, 0.75, 0.80, 0.85, 0.90, 0.95], ["70%", "75%", "80%", "85%", "90%", "95%"])
    ax.yaxis.set_major_locator(MaxNLocator(4, integer=True))
    titles(fig, t, "How sure the cameras were that a person was a person",
           f"{total:,} person detections during show hours (5-10 PM), Sep 12 to Nov 1")
    fig.subplots_adjust(left=0.07, right=0.98, top=0.74, bottom=0.12)
    save(fig, "detection-confidence", mode)


INCIDENTS = [  # (number, date, class) from docs/incident-reviews.md
    (1, "2026-09-23", "Process"), (2, "2026-09-23", "Software"), (3, "2026-09-24", "Software"),
    (4, "2026-09-24", "Process"), (5, "2026-09-25", "Firmware"), (6, "2026-09-26", "Firmware"),
    (7, "2026-09-27", "Detection & content"), (8, "2026-09-28", "Software"), (9, "2026-09-29", "Power & hardware"),
    (10, "2026-09-30", "Power & hardware"), (11, "2026-09-30", "Software"), (12, "2026-10-02", "Detection & content"),
    (13, "2026-10-20", "Network"), (14, "2026-10-27", "Process"), (15, "2026-10-27", "Power & hardware"),
    (16, "2026-10-28", "Power & hardware"), (17, "2026-11-05", "Firmware"),
]


def incidents(t, mode):
    classes = ["Power & hardware", "Software", "Firmware", "Process", "Detection & content", "Network"]
    counts = {c: sum(1 for _, _, k in INCIDENTS if k == c) for c in classes}
    d0 = date(2026, 9, 20)
    fig, ax = figure(t, 8.6, 3.9)
    ax.yaxis.grid(False)
    ax.xaxis.grid(True, color=t["grid"], linewidth=0.8)
    ax.spines["bottom"].set_color(t["axis"])
    rows = {c: len(classes) - 1 - i for i, c in enumerate(classes)}
    last = {}
    gap = 2.3                                          # days: keeps neighbouring dots from touching
    for n, d, k in INCIDENTS:
        x = (date.fromisoformat(d) - d0).days
        if k in last and x - last[k] < gap:
            x = last[k] + gap                          # nudge right, keep date order
        last[k] = x
        ax.scatter([x], [rows[k]], s=190, color=t["s1"], edgecolors=t["surface"], linewidths=2, zorder=3)
        ax.text(x, rows[k], str(n), ha="center", va="center", color="#ffffff", fontsize=7.5, zorder=4,
                fontweight="bold")
    ax.set_yticks([rows[c] for c in classes], [f"{c}  ({counts[c]})" for c in classes])
    ax.tick_params(axis="y", colors=t["ink2"], labelsize=9.5)
    ticks = [date(2026, 9, 21), date(2026, 9, 28), date(2026, 10, 5), date(2026, 10, 12), date(2026, 10, 19),
             date(2026, 10, 26), date(2026, 11, 2)]
    ax.set_xticks([(d - d0).days for d in ticks], [d.strftime("%b %-d") for d in ticks])
    show_start = (date(2026, 10, 20) - d0).days
    ax.axvspan(show_start, (date(2026, 11, 1) - d0).days, color=t["grid"], alpha=0.5, zorder=0)
    ax.text(show_start + 0.5, len(classes) - 0.45, "show nights", color=t["muted"], fontsize=8.5, va="bottom")
    ax.set_ylim(-0.7, len(classes) - 0.2)
    ax.set_xlim(0, (date(2026, 11, 9) - d0).days)
    titles(fig, t, "17 incidents, each root-caused and written up",
           "Numbered as in docs/incident-reviews.md (dots on the same days are spread out to stay readable).")
    fig.subplots_adjust(left=0.22, right=0.98, top=0.76, bottom=0.10)
    save(fig, "incidents", mode)


def main():
    for mode, t in THEMES.items():
        plt.rcParams["font.family"] = FONT
        nights(t, mode)
        halloween_hours(t, mode)
        funnel(t, mode)
        ai_cost(t, mode)
        ai_latency(t, mode)
        confidence(t, mode)
        incidents(t, mode)
    print("wrote", sorted(f for f in os.listdir(OUT) if f.endswith(".png")))


if __name__ == "__main__":
    main()
