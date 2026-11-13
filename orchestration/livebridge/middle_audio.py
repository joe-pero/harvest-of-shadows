"""
middle_audio.py — the live MIDDLE dialogue pipeline: one fast vision
call (OpenAI, see vision/openai_middle.py) reads the photo, then live XTTS-v2
synthesis (live_tts.py) speaks the actual line. Rate-limited, audited, and
gated by the bridge's arm switch.

STATUS 2026-09-18: Gemini support (the original provider, free-tier) was
removed once Joe funded a real OpenAI balance and no longer needed it — see
PROJECT_LOG.md "2026-09-18". OpenAI is the only vision/text provider now;
`GeminiGovernor` below is a holdover class name, fully provider-agnostic in
what it actually governs (cooldown/daily-cap/lifetime-$-cap/schedule, keyed
by call `kind`, not by provider).

Everything still fails SAFE and QUIET: any skip/error just means "no live
MIDDLE line this cycle" — the controller already falls back to
FALLBACK_TONE and firmware handles a missing MIDDLE file cleanly.

2026-09-24: the live AI OUTRO (generate_outro_lines, `_sc_outro`/`_bu_outro`
files, the "outro" cooldown kind) was RETIRED (Joe: outdated, not needed).
OUTRO is the pre-recorded Scarecrow/Burt banter; see docs/CURRENT-BUILD.md.

Real dollar cost per call is small (~$0.0005-0.0006, see
docs/_history/vision-cost-and-provider.md) but real — governed by LIFETIME_BUDGET_USD
below (a hard cap, not a rate limit) plus SCHEDULE_HOURS and the daily/
per-kind cooldowns. Joe: re-tune MIN_SECONDS_BETWEEN_CALLS /
MAX_CALLS_PER_DAY / LIFETIME_BUDGET_USD
below if real usage outpaces or undershoots them.
"""
from __future__ import annotations

import json
import os
import random
import sys
import time
from datetime import date, datetime
from pathlib import Path
from typing import TYPE_CHECKING

HERE = Path(__file__).resolve().parent
ORCH = HERE.parent
sys.path.insert(0, str(ORCH))

from showsim.constants import ShowMode, ShowTheme  # noqa: E402
from livebridge.frigate_events import fetch_snapshot  # noqa: E402
from livebridge import live_tts, middle_splice  # noqa: E402

if TYPE_CHECKING:
    from vision.middle_pipeline import MiddleResult

STATE_DIR = HERE / "state"
USAGE_FILE = STATE_DIR / "gemini_usage.json"
LIFETIME_FILE = STATE_DIR / "gemini_lifetime_spend.json"

AUDIO_DIR = Path("/tmp/halloween_audio")
MIDDLE_DIR = AUDIO_DIR / "middle"

# The pre-rendered bank — output of `voice/scripts/render_pools.sh scarecrow
# middle` (per-bucket text: `voice/lines/scarecrow/middle/<bucket>.txt`).
BANK_DIR = ORCH / "voice" / "out" / "pools" / "scarecrow" / "middle"
ANTI_REPEAT_LAST_N = 3   # don't replay one of the last N indices played in a bucket

# ── tunable safety constants — see module docstring ─────────────────────────
MIN_SECONDS_BETWEEN_CALLS = 15.0   # <= 4 calls/min — under every reported RPM floor;
                                   # applies to "middle"-kind calls (the vision read +
                                   # its optional second look)
MAX_CALLS_PER_DAY = 1000           # 2026-10-31 RAISED from 300 for Halloween (Joe: "run
                                   # it until we run out"); the lifetime $ cap below is
                                   # the real backstop. RAISED 2026-09-19 from 40 — that number was
                                   # sized for the old ~1-2-calls/visit world;
                                   # adding Burt-flavor + the outro pair raised a
                                   # full visit to ~3-4 real calls. Joe's own
                                   # stated volume estimate (docs/vision-cost-and-
                                   # provider.md, "double digits (~20-60) on the
                                   # night itself") means the busiest realistic
                                   # night needs up to 60*4=240 calls -- the old
                                   # cap of 40 would have silently gone dark after
                                   # ~10 visitors. 300 gives real headroom above
                                   # that while still being a genuine, meaningful
                                   # cap (this constant's real job today is a
                                   # runaway-bug backstop, not $ protection --
                                   # lifetime $ cap is the real backstop either way.
DAILY_WARN_AT = 0.8                # log loudly once >= 80% of the daily cap is used
BACKOFF_AFTER_429_S = 300.0        # if Google itself says "slow down", stand down 5 min

# 2026-09-18: only fire live AI calls during actual expected trick-or-treat
# hours, regardless of arm state otherwise -- Joe: "I can't have it running
# all day and all night." Keyed by Python's date.weekday() (Mon=0 .. Sun=6).
# Sun-Thu 5-8pm, Fri-Sat 5-9pm (his numbers, close to Halloween). Outside the
# window: same as disarmed for the AI-vision feature specifically (cues/props
# still work if separately armed -- this only gates the paid call).
SCHEDULE_HOURS = {
    6: (17, 20),  # Sunday
    0: (17, 20),  # Monday
    1: (17, 20),  # Tuesday
    2: (17, 20),  # Wednesday
    3: (17, 20),  # Thursday
    4: (17, 21),  # Friday
    5: (17, 22),  # Saturday -- 2026-10-31: Halloween to 22:00 (Joe stops ~10 PM)
}

# 2026-09-18: a REAL dollar cap, separate from the daily call-count cap above.
# Added once Joe funded a real $20 OpenAI balance for this feature — every
# call here has an actual cost, and the daily
# counter alone resets every midnight so it can't protect a one-time budget
# that's meant to last the whole season. This is lifetime, never resets, and
# trips a hard force_disarm() (not just a skip) so the whole show stops, not
# just this one feature — matches Joe's "I need to ensure my money is not
# lost" bar. LIFETIME_BUDGET_USD is set with headroom under the real $20.
LIFETIME_BUDGET_USD = 18.00
# 2026-10-31: spend is now charged from the REAL token usage of each call
# (vision/openai_middle.py MODEL_PRICES_PER_M -> result.diag["cost_usd"]).
# The old flat $0.0006 undercounted Luna ~2x (reasoning tokens bill as
# output) and would undercount Sol ~40x. These per-model figures are only
# charged when a call returns no usage (crash/exception): ~1.3x measured.
FALLBACK_COST_PER_CALL_USD = {"gpt-5.6-luna": 0.0015, "gpt-5.6-terra": 0.012, "gpt-5.6-sol": 0.03}

# 2026-10-31: which model the live MIDDLE call uses, read on EVERY call so it
# can change mid-show with no bridge restart (`python3 tools/ai_model.py
# heavy|normal|status`). Missing file = the normal model. A model without a
# known price is refused (falls back to normal, logged loud).
AI_MODEL_FILE = STATE_DIR / "ai_model.json"
NORMAL_MODEL = "gpt-5.6-luna"
HEAVY_MODEL = "gpt-5.6-sol"


def active_model(loud=print) -> str:
    try:
        model = json.loads(AI_MODEL_FILE.read_text()).get("model") or NORMAL_MODEL
    except FileNotFoundError:
        return NORMAL_MODEL
    except Exception as e:
        loud(f"ai_model.json unreadable ({e!r}) -- using {NORMAL_MODEL}")
        return NORMAL_MODEL
    if model not in FALLBACK_COST_PER_CALL_USD:
        loud(f"ai_model.json names {model!r} (no known price) -- using {NORMAL_MODEL}")
        return NORMAL_MODEL
    return model
LIFETIME_WARN_AT = 0.8      # log loudly once >= 80% of the lifetime budget is used

_THEME_TO_PIPELINE = {ShowTheme.CORN: "corn", ShowTheme.ROTLD: "rotld", ShowTheme.MUTANT_PUMPKINS: "pumpkins"}


class GeminiGovernor:
    """Owns the cooldown / daily-cap / lifetime-spend / backoff state and the
    audit trail. Persists the daily counter AND the lifetime spend to disk so
    a bridge restart doesn't reset either one. Audit rows go through the
    injected ShowLog (fire-and-forget, unified with the rest of the
    pipeline's structured log) rather than a separate file/writer of its
    own."""

    def __init__(self, log=print, loud=None, show_log=None, disarm_fn=None) -> None:
        self._log = log
        self._loud = loud or (lambda m: log(f"!!! {m}"))
        self._show_log = show_log
        # 2026-09-18: calls back into ShowMQTT.force_disarm() when the
        # lifetime spend cap trips — a real $ cap needs to stop the whole
        # show, not just silently skip this one feature's calls forever.
        self._disarm_fn = disarm_fn or (lambda reason: None)
        self._last_call_at: dict[str, float] = {}   # keyed by call "kind" (currently only "middle")
        self._backoff_until: float = 0.0
        STATE_DIR.mkdir(parents=True, exist_ok=True)
        self._daily = self._load_daily()
        self._lifetime_usd = self._load_lifetime()
        if self._lifetime_usd >= LIFETIME_BUDGET_USD:
            # Cap was already tripped in a previous run -- re-assert disarm
            # on every boot so a stale retained "armed" can't slip through
            # before the first call attempt would have caught it.
            self._disarm_fn(f"lifetime spend ${self._lifetime_usd:.2f} already "
                            f">= ${LIFETIME_BUDGET_USD:.2f} cap (from a previous run)")

    def _load_daily(self) -> dict:
        today = date.today().isoformat()
        try:
            d = json.loads(USAGE_FILE.read_text())
            if d.get("date") == today:
                return d
        except Exception:
            pass
        return {"date": today, "count": 0}

    def _save_daily(self) -> None:
        USAGE_FILE.write_text(json.dumps(self._daily))

    def _roll_day_if_needed(self) -> None:
        today = date.today().isoformat()
        if self._daily.get("date") != today:
            self._daily = {"date": today, "count": 0}
            self._save_daily()

    def _load_lifetime(self) -> float:
        try:
            return float(json.loads(LIFETIME_FILE.read_text()).get("usd", 0.0))
        except Exception:
            return 0.0

    def _save_lifetime(self) -> None:
        LIFETIME_FILE.write_text(json.dumps({"usd": self._lifetime_usd}))

    @staticmethod
    def _within_schedule(now_dt: datetime | None = None) -> tuple[bool, str]:
        now_dt = now_dt or datetime.now()
        window = SCHEDULE_HOURS.get(now_dt.weekday())
        if window is None:
            return False, "no_schedule_for_today"
        start_h, end_h = window
        if start_h <= now_dt.hour < end_h:
            return True, ""
        return False, f"outside_schedule (window {start_h}:00-{end_h}:00, now {now_dt.strftime('%H:%M')})"

    _MIN_SECONDS_BY_KIND = {"middle": MIN_SECONDS_BETWEEN_CALLS}

    def allowed(self, kind: str = "middle", ignore_cooldown: bool = False) -> tuple[bool, str]:
        """(True, "") or (False, reason) — call BEFORE every OpenAI
        attempt. `kind` picks which cooldown floor applies (see
        _MIN_SECONDS_BY_KIND) — daily cap / lifetime cap / schedule / 429
        backoff are shared across every kind, since those are real global
        spend limits, not per-feature pacing."""
        self._roll_day_if_needed()
        if self._lifetime_usd >= LIFETIME_BUDGET_USD:
            return False, f"lifetime_cost_cap (${self._lifetime_usd:.2f}/${LIFETIME_BUDGET_USD:.2f})"
        in_window, why = self._within_schedule()
        if not in_window:
            return False, why
        now = time.time()
        if now < self._backoff_until:
            return False, f"backoff_after_429 ({self._backoff_until - now:.0f}s left)"
        min_gap = self._MIN_SECONDS_BY_KIND.get(kind, MIN_SECONDS_BETWEEN_CALLS)
        last = self._last_call_at.get(kind)
        if not ignore_cooldown and last is not None and (now - last) < min_gap:
            return False, f"cooldown_{kind} ({min_gap - (now - last):.0f}s left)"
        if self._daily["count"] >= MAX_CALLS_PER_DAY:
            return False, f"daily_cap ({self._daily['count']}/{MAX_CALLS_PER_DAY})"
        return True, ""

    def record_attempt(self, kind: str = "middle") -> None:
        self._last_call_at[kind] = time.time()
        self._daily["count"] += 1
        self._save_daily()
        frac = self._daily["count"] / MAX_CALLS_PER_DAY
        if frac >= DAILY_WARN_AT:
            self._loud(f"AI daily usage at {self._daily['count']}/{MAX_CALLS_PER_DAY} ({frac:.0%})")

    def charge(self, usd: float) -> None:
        """Add a call's real $ cost (after it answers) to the lifetime spend."""
        was_under = self._lifetime_usd < LIFETIME_BUDGET_USD
        self._lifetime_usd += usd
        self._save_lifetime()
        lifetime_frac = self._lifetime_usd / LIFETIME_BUDGET_USD
        if lifetime_frac >= LIFETIME_WARN_AT:
            self._loud(f"Lifetime AI-vision spend at ${self._lifetime_usd:.2f}/"
                      f"${LIFETIME_BUDGET_USD:.2f} ({lifetime_frac:.0%})")
        if was_under and self._lifetime_usd >= LIFETIME_BUDGET_USD:
            self._disarm_fn(f"lifetime spend ${self._lifetime_usd:.2f} reached the "
                            f"${LIFETIME_BUDGET_USD:.2f} cap")

    def status(self) -> dict:
        """Live AI spend/gate snapshot for the dashboard (published retained on
        harvest/bridge/ai_status by service.py). Read-only; changes nothing."""
        self._roll_day_if_needed()
        in_window, why = self._within_schedule()
        now = time.time()
        if self._lifetime_usd >= LIFETIME_BUDGET_USD:
            gate = "budget_cap"
        elif not in_window:
            gate = "outside_hours"
        elif now < self._backoff_until:
            gate = "backoff"
        elif self._daily["count"] >= MAX_CALLS_PER_DAY:
            gate = "daily_cap"
        else:
            gate = "open"
        win = SCHEDULE_HOURS.get(datetime.now().weekday())
        return {
            "gate": gate,
            "window": f"{win[0]}:00-{win[1]}:00" if win else None,
            "calls_today": self._daily["count"],
            "daily_cap": MAX_CALLS_PER_DAY,
            "spend_usd": round(self._lifetime_usd, 4),
            "cap_usd": LIFETIME_BUDGET_USD,
            "backoff_s": max(0, round(self._backoff_until - now)),
            "last_call_at": round(max(self._last_call_at.values())) if self._last_call_at else None,
            "model": active_model(self._loud),
        }

    def record_429(self) -> None:
        self._backoff_until = time.time() + BACKOFF_AFTER_429_S
        self._loud(f"OpenAI returned a rate-limit error — backing off {BACKOFF_AFTER_429_S:.0f}s")

    def audit(self, **fields) -> None:
        if self._show_log is not None:
            self._show_log.log("gemini_call", **fields)


class MiddleBank:
    """The pre-rendered, per-(theme,tone) MIDDLE line pool. Read-only at
    runtime — rendering happens offline via render_pools.sh. Rescans lazily
    so a render finishing while the bridge is already running (exactly
    tonight's situation) gets picked up without a restart."""

    def __init__(self, log=print) -> None:
        self._log = log
        self._cache: dict[str, list[Path]] = {}
        self._mtime: dict[str, float] = {}
        self._recent: dict[str, list[int]] = {}

    def _bucket_key(self, theme: ShowTheme, tone: ShowMode) -> str:
        t = tone if tone in (ShowMode.FRIENDLY, ShowMode.SCARY) else ShowMode.SCARY
        return f"{_THEME_TO_PIPELINE[theme]}_{t.name.lower()}"

    def _files_for(self, bucket: str) -> list[Path]:
        d = BANK_DIR / bucket
        try:
            mtime = d.stat().st_mtime
        except FileNotFoundError:
            return []
        if self._cache.get(bucket) is not None and self._mtime.get(bucket) == mtime:
            return self._cache[bucket]
        # prefer the fully post-processed "_show" file, same as SD-card audio;
        # fall back to "_raw_fx" (fx-treated but not yet show-processed), then raw.
        files = sorted(d.glob("*_show.wav"))
        if not files:
            files = sorted(d.glob("*_raw_fx.wav"))
        if not files:
            files = sorted(d.glob("*_raw.wav"))
        self._cache[bucket] = files
        self._mtime[bucket] = mtime
        return files

    def pick(self, theme: ShowTheme, tone: ShowMode) -> Path | None:
        bucket = self._bucket_key(theme, tone)
        files = self._files_for(bucket)
        if not files:
            self._log(f"[middle] bank empty for {bucket} (not rendered yet?) — no MIDDLE line this cycle")
            return None
        recent = self._recent.setdefault(bucket, [])
        choices = [i for i in range(len(files)) if i not in recent] or list(range(len(files)))
        idx = random.choice(choices)
        recent.append(idx)
        del recent[:-ANTI_REPEAT_LAST_N]
        return files[idx]


class MiddleAudioService:
    """The `waiting_zone_hint_fn(event_id, theme, session_id) -> ShowMode|None`
    callback frigate_events.py calls."""

    def __init__(self, log=print, loud=None, show_log=None, disarm_fn=None,
                 armed_fn=None, no_middle_fn=None) -> None:
        # 2026-09-25: called with session_id when a MIDDLE that params promised
        # (middle=true) can't be produced -> ShowMQTT.publish_nomiddle.
        self._no_middle_fn = no_middle_fn or (lambda session_id: None)
        # 2026-09-24: the dashboard ARMED/OFF switch gates paid calls. The
        # second look now runs in the background AFTER PRE-SHOW starts, so it
        # must re-check the switch itself (OFF pressed mid-visit = no new call).
        self._armed_fn = armed_fn or (lambda: True)
        self._log = log
        self._loud = loud or (lambda m: log(f"!!! {m}"))
        self._governor = GeminiGovernor(log=log, loud=loud, show_log=show_log,
                                        disarm_fn=disarm_fn)
        self._bank = MiddleBank(log=log)
        # One worker: live synth jobs serialize (one XTTS model, one voice at a time).
        from concurrent.futures import ThreadPoolExecutor
        self._synth_pool = ThreadPoolExecutor(max_workers=1, thread_name_prefix="middle-synth")

    def ensure_loaded(self) -> bool:
        """Checks the OpenAI key is present, then warms the live-TTS model
        (2026-09-17: live synthesis is back, see live_tts.py's docstring for
        why). The XTTS-v2 load (~53s) happens here, once, at bridge startup
        — never on a real visitor's request. A warm-up failure is logged but
        non-fatal: hint_for() falls back to the pre-rendered bank per-call
        if live synthesis isn't available."""
        if not os.environ.get("OPENAI_API_KEY"):
            self._log("[middle] no OPENAI_API_KEY set — MIDDLE pipeline stays off")
            return False
        live_tts.warm_up(log=self._log)
        return True

    def ai_status(self) -> dict:
        return self._governor.status()

    def close(self) -> None:
        self._synth_pool.shutdown(wait=False, cancel_futures=True)

    def hint_for(self, event_id: str, theme: ShowTheme, session_id: int) -> ShowMode | None:
        """The callback itself. Never raises — every failure path degrades
        to `return None` (FALLBACK_TONE) after an audit-log entry."""
        # 2026-09-28: the trigger can now come from the FREE show switch with
        # the AI switch off -- no paid call then; the prerecorded path plays.
        if not self._armed_fn():
            allowed, reason = False, "AI switch off (FREE show: prerecorded only)"
        else:
            allowed, reason = self._governor.allowed()
        if not allowed:
            self._log(f"[middle] skip (session={session_id:08x}): {reason}")
            self._governor.audit(session_id=session_id, event_id=event_id,
                                 decision="skip", reason=reason)
            return None

        if not self.ensure_loaded():
            self._governor.audit(session_id=session_id, event_id=event_id,
                                 decision="skip", reason="no_api_key")
            return None

        snapshot = fetch_snapshot(event_id)
        if snapshot is None:
            self._log(f"[middle] skip (session={session_id:08x}): snapshot fetch failed")
            self._governor.audit(session_id=session_id, event_id=event_id,
                                 decision="skip", reason="no_snapshot")
            return None

        self._governor.record_attempt()
        t0 = time.monotonic()
        model = active_model(self._loud)
        result = self._call_vision(snapshot, theme, model)
        if result is not None and (getattr(result, "diag", None) or {}).get("attempts", 1) > 1:
            self._governor.record_attempt()          # the internal retry was a real billed call
        self._charge_call(result, model)
        gemini_ms = (time.monotonic() - t0) * 1000

        if result is None:                       # _call_vision's own except fired --
            # a crash outside generate_middle_openai()'s internal handling (bad client
            # construction, etc.), NOT the common 429/network case -- those are
            # already caught inside generate_middle_openai() itself and come
            # back as a normal (but ok=False) MiddleResult, handled below with
            # the real error text preserved.
            self._governor.audit(session_id=session_id, event_id=event_id,
                                 decision="internal_error", gemini_ms=round(gemini_ms))
            return None

        # 2026-09-24 (#2): tone is the model's real read; the controller
        # locks the whole cycle (INTRO/MIDDLE/HOLD/OUTRO) to it.
        # 2026-09-24 hardening: the template MIDDLE doesn't use villain_line, so
        # a rejected villain_line must not throw away an otherwise-good read
        # (tone, counts, clothing_insert are screened on their own). Only a
        # truly failed call (API error, bad JSON) means "no observation".
        observed = result.ok or result.fallback_reason.startswith("line_rejected")
        hint = self._tone_to_showmode(result.tone) if observed else None
        # 2026-09-24 hardening (root-caused from real logs: 196-243s of silence
        # before PRE-SHOW). frigate_events only fires PRE-SHOW (the INTRO cue)
        # AFTER this function returns, so everything slow MUST NOT happen here.
        # Return the tone the moment the vision call answers; the second look,
        # live insert synthesis, splice and Burt's line run on a background
        # worker DURING the INTRO (Joe: "queue it up during the intro to fire
        # as soon as it is available"). The result lands as
        # middle/<session>_sc.wav whenever it's ready; firmware picks it up.
        if hint is None:
            self._produce(result, event_id, theme, session_id, gemini_ms, hint)
            return None
        self._synth_pool.submit(self._produce_safe, result, event_id, theme,
                                session_id, gemini_ms, hint)
        self._log(f"[middle] session={session_id:08x} tone ready in {gemini_ms:.0f}ms "
                  f"-> PRE-SHOW can start; assembly queued")
        return hint

    def _produce_safe(self, *args) -> None:
        try:
            self._produce(*args)
        except Exception as e:                      # background thread: never die silently
            self._loud(f"MIDDLE background assembly crashed: {e!r}")
            self._no_middle(args[3])

    def _no_middle(self, session_id: int) -> None:
        self._loud(f"MIDDLE promised for session={session_id:08x} but none produced -- sending nomiddle")
        try:
            self._no_middle_fn(session_id)
        except Exception as e:
            self._loud(f"nomiddle publish failed: {e!r}")

    def _produce(self, result: "MiddleResult", event_id: str, theme: ShowTheme,
                 session_id: int, gemini_ms: float, hint: ShowMode | None) -> None:
        """Slow half of the MIDDLE pipeline (runs on the synth worker):
        optional second look, template assembly or bank fallback, Burt's line,
        audit. Never blocks PRE-SHOW."""
        if hint is not None:
            result = self._maybe_second_look(result, event_id, theme) or result
        placed = False
        live = False
        live_parts = 0
        template = None           # (bucket, index) actually used, for the audit log
        if hint is not None:
            # 2026-09-24 new build (docs/CURRENT-BUILD.md section 2): Scarecrow's
            # MIDDLE is [headcount clip] + pre-recorded template part A + ONE
            # short live clothing/props insert + part B. Only the insert is
            # synthesized live. Replaces the full-line live villain_line
            # synthesis (29-243s), which could never fit the <=30s post-INTRO
            # wait. If the template path can't run (insert rejected, bucket
            # not rendered yet, synth failed) the instant pre-rendered bank
            # below is the only fallback -- no slow full-line retry.
            burt_text = None
            if result.clothing_insert:
                bucket = f"{_THEME_TO_PIPELINE[theme]}_{result.tone}"
                pick = middle_splice.pick_template(bucket, result.clothing_insert)
                if pick is None:
                    self._log(f"[middle] no rendered template in {bucket} fits "
                              f"{result.clothing_insert!r} yet -- bank")
                else:
                    idx, sc_text, burt_text = pick
                    MIDDLE_DIR.mkdir(parents=True, exist_ok=True)
                    out_path = MIDDLE_DIR / f"{session_id:08x}_sc.wav"
                    t1 = time.monotonic()
                    live = middle_splice.splice_middle_line(
                        "scarecrow", bucket, idx, sc_text, out_path,
                        log=self._log, headcount_key=result.headcount_key)
                    synth_ms = (time.monotonic() - t1) * 1000
                    live_parts = 1 if live else 0
                    template = f"{bucket}#{idx:02d}"
                    if live:
                        placed = True
                        self._log(f"[middle] template {template} ok in {synth_ms:.0f}ms: "
                                  f"headcount={result.headcount_key} insert={sc_text!r}")
                    else:
                        self._log(f"[middle] template {template} failed after "
                                  f"{synth_ms:.0f}ms -- bank")
            else:
                self._log(f"[middle] insert unusable ({result.insert_reject_reason}) -- bank")
            if not placed:
                placed = self._place_from_bank(theme, hint, session_id)
            if not placed:
                self._no_middle(session_id)

            # 2026-09-24 (#3, Joe): Burt's MIDDLE is his MATCHED template --
            # line N of burt/middle_templates/<bucket> is written as his
            # reaction to Scarecrow's line N, with the same clothing insert
            # (fitted to Burt's own sentence). It only makes sense after
            # Scarecrow's line N actually played, so: Scarecrow template
            # played -> Burt's matched line; otherwise Burt gets nothing extra
            # (his SD preshow line, as always). No separate AI call, no headcount.
            if live and burt_text:
                burt_path = MIDDLE_DIR / f"{session_id:08x}_bu.wav"
                t2 = time.monotonic()
                burt_live = middle_splice.splice_middle_line(
                    "burt", bucket, idx, burt_text, burt_path, log=self._log)
                self._log(f"[middle] burt matched {template} "
                          f"{'ok' if burt_live else 'failed'} in "
                          f"{(time.monotonic() - t2) * 1000:.0f}ms: insert={burt_text!r}")
            elif live:
                self._log(f"[middle] burt's matched line for {template} not rendered/"
                          f"fitting yet -- Burt plays his SD line")

        # error_class lets a later audit-log query group failures by real cause
        # (503 vs 429 vs other) instead of everything reading "api_error" --
        # added 2026-09-17 alongside the live-synth work so we can actually
        # measure the real API 503 "high demand" rate before deciding
        # whether a paid tier would fix it or not.
        error_class = None
        if result.fallback_reason == "api_error" and result.error:
            e = result.error
            error_class = ("503" if "503" in e or "UNAVAILABLE" in e
                          else "429" if "429" in e or "RESOURCE_EXHAUSTED" in e
                          else "other")
        self._governor.audit(
            session_id=session_id, event_id=event_id, decision="called",
            gemini_ms=round(gemini_ms), ok=result.ok, tone=result.tone,
            tone_reason=result.tone_reason, fell_back=result.fell_back,
            fallback_reason=result.fallback_reason, error=result.error,
            error_class=error_class, placed=placed, live=live, live_parts=live_parts,
            villain_line=result.villain_line, costume_summary=result.costume_summary,
            template=template, headcount_key=result.headcount_key,
            burt_matched=bool(live and burt_text),
            api_diag=getattr(result, "diag", None),
            clothing_insert=result.clothing_insert,
            insert_reject_reason=result.insert_reject_reason,
        )
        self._log(
            f"[middle] session={session_id:08x} done: ok={result.ok} tone={result.tone} "
            f"({result.tone_reason}) placed={placed} live={live} live_parts={live_parts} "
            f"gemini_ms={gemini_ms:.0f}"
        )

    @staticmethod
    def _tone_to_showmode(tone: str) -> ShowMode | None:
        return {"friendly": ShowMode.FRIENDLY, "scary": ShowMode.SCARY}.get(tone)

    SECOND_LOOK_MIN_SUMMARY_LEN = 20   # below this, treat costume_summary as "found nothing"
    SECOND_LOOK_WAIT_S = 3.0            # let the visitor get closer/clearer before re-looking

    def _maybe_second_look(self, first_result: "MiddleResult | None",
                           event_id: str, theme: ShowTheme) -> "MiddleResult | None":
        """2026-09-18: if the first read didn't find anything distinctive
        (a short/empty costume_summary -- the real bug behind the "bright
        trinket" mis-ID from earlier tonight, where the model only ever saw "a
        pink object"), take ONE more look a few seconds later. The visitor
        is usually closer/clearer by then, especially now that the pipeline
        has real time margin (this call already has no latency deadline --
        it's designed to run up to the full ~90-120s budget regardless).

        This is an accuracy pass on the SAME visit, not a new independent
        trigger, so it deliberately does NOT re-check the governor's
        per-trigger cooldown -- but it DOES call record_attempt() again,
        since it's a real second billed call and must count against the
        daily/lifetime caps like any other attempt. Runs on the existing
        single-worker background thread (frigate_events.py's
        ThreadPoolExecutor) -- blocking here for a few seconds is safe and
        already how long these calls normally take.
        """
        if first_result is None or not first_result.ok:
            return first_result
        summary = (first_result.costume_summary or "").strip()
        has_insert = bool(first_result.clothing_insert)
        if len(summary) >= self.SECOND_LOOK_MIN_SUMMARY_LEN and has_insert:
            return first_result   # already found something concrete AND usable

        # Same visit, not a new trigger: skip ONLY the per-trigger cooldown
        # (as this docstring always intended -- the plain allowed() call here
        # blocked every second look, since it fires 3-8s after the first call,
        # inside the 15s cooldown). Daily cap, lifetime $ cap, schedule and
        # 429 backoff all still apply.
        if not self._armed_fn():
            self._log("[middle] second look skipped: bridge DISARMED since the first read "
                      "(no paid call)")
            return first_result
        allowed, reason = self._governor.allowed(ignore_cooldown=True)
        if not allowed:
            self._log(f"[middle] first read weak ({summary!r}) but second look "
                      f"blocked: {reason}")
            return first_result

        self._log(f"[middle] first read weak/unusable (summary={summary!r}, "
                  f"insert={'yes' if has_insert else 'no'}) — taking a second "
                  f"look in {self.SECOND_LOOK_WAIT_S:.0f}s")
        time.sleep(self.SECOND_LOOK_WAIT_S)
        snapshot2 = fetch_snapshot(event_id)
        if snapshot2 is None:
            return first_result

        self._governor.record_attempt()
        model = active_model(self._loud)
        second_result = self._call_vision(snapshot2, theme, model)
        self._charge_call(second_result, model)
        if second_result is not None and second_result.ok:
            second_summary = (second_result.costume_summary or "").strip()
            # A usable insert outranks a longer summary: it's what the show plays.
            if second_result.clothing_insert and not has_insert:
                self._log(f"[middle] second look produced a usable insert: "
                          f"{second_result.clothing_insert!r}")
                return second_result
            if len(second_summary) > len(summary) and (second_result.clothing_insert or not has_insert):
                self._log(f"[middle] second look found more: {second_summary!r}")
                return second_result
            self._log("[middle] second look no better — keeping the first read")
        return first_result

    def _charge_call(self, result, model: str) -> None:
        """Real cost from the call's token usage; the per-model fallback
        figure only when the call returned no usage."""
        usd = (getattr(result, "diag", None) or {}).get("cost_usd") if result is not None else None
        self._governor.charge(usd if usd else FALLBACK_COST_PER_CALL_USD[model])

    def _call_vision(self, snapshot: bytes, theme: ShowTheme,
                     model: str = NORMAL_MODEL) -> "MiddleResult | None":
        """The one live vision call. OpenAI only (Gemini support removed
        2026-09-18 — see PROJECT_LOG.md "2026-09-18"; Joe funded a real
        OpenAI balance and no longer needed the free-tier fallback). Same
        MiddleResult contract, same parse_response()/_screen() safety
        enforcement, same GeminiGovernor gating (cooldown/daily-cap/
        lifetime-$-cap/schedule) as before — none of that changed, only the
        network call did. 2026-09-24 (#2, Joe): tone FOLLOWS the model's
        real read (TONE_RULES: young child / stroller -> friendly, all 10+ ->
        scary, uncertain -> FALLBACK_TONE). The old manual_tone="scary" lock
        overwrote the read, so the AI could never make a visit friendly."""
        from vision.openai_middle import generate_middle_openai
        try:
            return generate_middle_openai(snapshot, "scarecrow",
                                          _THEME_TO_PIPELINE[theme], model=model)
        except Exception as e:
            msg = str(e)
            if "429" in msg or "rate" in msg.lower():
                self._governor.record_429()
            else:
                self._loud(f"OpenAI call raised: {e!r}")
            return None

    def _place_from_bank(self, theme: ShowTheme, tone: ShowMode, session_id: int) -> bool:
        src = self._bank.pick(theme, tone)
        if src is None:
            return False
        MIDDLE_DIR.mkdir(parents=True, exist_ok=True)
        out_path = MIDDLE_DIR / f"{session_id:08x}_sc.wav"
        try:
            import shutil
            shutil.copyfile(src, out_path)
        except Exception as e:
            self._loud(f"bank copy failed ({src} -> {out_path}): {e!r}")
            return False
        return True
