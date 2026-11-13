# Operating principles

Cross-cutting rules for working on this project. Prop-specific detail lives in
each prop's own doc; this file is the "how we work" layer.

---

## OP-1 — Resource check before any heavy local job (the "freed machine" rule)

**Applies to:** anything that adds real load to the Workshop PC — model inference,
fine-tuning / training, large media transcodes, bulk audio processing, Frigate
re-tuning, anything that will hold multiple GB of RAM or peg the CPU for minutes+.

**Why this exists:** two separate failures on the Workshop PC traced to the same
cause — not enough headroom on the machine for the load being added, and nobody
noticing until it was too late:

1. **May 2026 Burt XTTS fine-tune** — claimed 6 epochs; the "best" checkpoint was
   later found byte-identical to its own epoch-0 save and the training log stops
   mid-epoch-1 with no traceback. It was SIGKILLed by the kernel OOM killer and
   *silently* produced a junk model that sat around for months looking real.
2. **Sept 2026 zero-shot inference test** — a genuinely lightweight job (~4 GB)
   that still took the machine down, because the box was already at ~6.7 GB free
   with swap full from a VM + Chrome + VS Code + several Claude sessions.
3. **2026-09-08 HOLD-pool render** — a 600-line render started at ~8.3 GB
   MemAvailable (Chrome + VS Code + Claude open), ran **log-only, not `--guard`**
   because "4.4 GB on ~8 GB is fine". It was: the *render* finished clean. But it
   pushed ~1.1 GB into swap, and the kernel evicted gnome-shell's pages to get
   it. That tripped a latent gnome-shell Clutter bug (`StDrawingArea` tried to
   allocate `-2147483648 x -2147483648` — an INT_MIN size) which **froze the
   desktop GUI for ~90 minutes** (mouse + keyboard dead; X11 and gnome-shell
   processes alive the whole time; recovered on its own as RAM freed). Lesson:
   the job surviving is not the only thing that matters — **swap pressure from a
   "safe" job can still take the desktop down.** `scripts/safe_render.sh` (2026-09-23)
   pre-flight-gates on MemAvailable, stops livebridge's resident XTTS model, and
   runs memwatch with `--guard`; `xtts_clone.py` refuses a second XTTS loader.

Same lesson every time: **"it worked before" is not evidence it is safe now**, a
one-time check at the start is not enough, and "the job didn't get killed" is not
the same as "nothing broke".

### The rule

**Before** starting the job:

- Close the heavy stuff first — VM, Chrome, VS Code, extra Claude / chat sessions.
- `free -h`: confirm **MemAvailable is comfortably above the job's known peak**,
  with generous margin — not merely "more than last time". If the job needs
  ~10–14 GB, "13 GB free" is *not* comfortable on a 16 GB box; say so and stop.
- `swapon --show`: swap should be near-empty going in.
- If the margin is thin, **stop and report** with the numbers. Do not start on the
  assumption it will probably be fine.

**During** the job:

- Run it under `scripts/memwatch.py` (in `orchestration/voice/scripts/`, but
  general-purpose) — periodic memory logging + a guard that SIGSTOPs the job on
  thin margin and either lets it recover or brings it down cleanly, instead of
  letting the kernel OOM-kill it (or swap out the desktop) with no warning.
  **Use `--guard`, not log-only** — the 2026-09-08 incident is exactly what
  log-only misses. For an audio-pool render, `scripts/safe_render.sh` already
  wires this (`--pause-mb 3000 --resume-mb 4000 --swap-abort-mb 1500`, memwatch
  in its own process group via `setsid` so it can't stop itself).
- `--guard` pauses on **MemAvailable**, not swap size — the goal is that the
  kernel never has to reclaim the desktop's working set in the first place. Pick
  `--pause-mb` so the desktop's resident set (gnome-shell + Xorg + your browser)
  stays untouched: ~3 GB headroom on this box.
- For training specifically, also cap it with a cgroup:
  `systemd-run --scope -p MemoryHigh=<n>G ...` so reclaim pressure hits the job
  before the whole machine thrashes.

**After** the job:

- Verify it actually did what it claims — see OP-2.

### Known machine limits (Workshop PC, as of 2026-09-05)

- Intel i5-7400, **4 cores / 4 threads**, **~15.5 GiB usable RAM**, Intel HD 630
  iGPU only — **no CUDA GPU**. All ML runs on CPU in system RAM.
- Swap: 2 GiB `/swapfile` by default — far too small to backstop a training spike.
  A dedicated large swapfile on the SSD (`/dev/sda3`, Fanxiang 512 GB) is the right
  backstop for transient spikes; never swap onto the backup HDD (`/dev/sdb`).
- Zero-shot XTTS-v2 inference: measured peak ~4.3 GB RSS — safe on a freed box.
- XTTS-v2 GPT fine-tune (518 M-param model, fp32, AdamW): the dominant cost is
  params + grads + optimizer state (~7 GB) **plus** a transient ~5.6 GB spike each
  time a checkpoint is written. The stock `xtts_ft_demo` config also spawns
  **8 dataloader workers** — cut to 1–2 on this machine. See
  `orchestration/voice/README.md`.

---

## OP-2 — Completion verification (don't trust the "done" message)

A job is not done because it printed "done" or a file exists. For model training:

- **Full epoch count actually reached** — grep the trainer log for the last
  `EPOCH: N/M` and confirm N hit M, with training steps logged *after* the last
  epoch boundary.
- **Final checkpoint differs from an early one** — `cmp` the final `best_model.pth`
  against the first `best_model_*.pth`. Byte-identical = the run did nothing.
- **Eval loss actually moved** — pull the eval-loss series from the log / TF events
  and confirm it decreased, not just "a checkpoint was saved".
- **Listen test** — synthesize a fixed set of lines and actually listen before
  calling a voice model usable.

Record the outcome (pass or fail, with the numbers) in the prop doc and
`PROJECT_LOG.md`.

---

## OP-3 — Session log

Append a short dated entry to `PROJECT_LOG.md` at the end of every work session on
this project, whatever the subsystem. The detailed write-up goes in `docs/`; the
log is just the timeline spine.
