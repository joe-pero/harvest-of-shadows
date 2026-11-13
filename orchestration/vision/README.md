# orchestration/vision — shared prompt/schema/safety library

Shared library, not a runnable tool. `middle_pipeline.py` owns everything a
provider call must never reimplement on its own: `CHARACTER_BIBLE`,
`CONTENT_BOUNDARIES`, `SEAM_RULES`/`OUTRO_RULES`, the response schema field
list, and `parse_response()`'s `_screen()` safety backstop. The only provider is
**OpenAI** (`openai_middle.py`, model `gpt-5.6-luna`). Current show design:
`../../docs/CURRENT-BUILD.md`.

- **Pending change:** `_proposed/2026-09-24_headcount_insert.patch` adds
  `adult_count` / `kid_count` / `clothing_insert` + `_screen_insert()` for the
  new template+insert MIDDLE. Drafted and offline-tested, **not applied**.
- **Code:** `middle_pipeline.py` (shared) · `openai_middle.py` (live calls)
- **Offline tests:** `test_screen.py` — no API key, no network. Re-run any
  time the prompt/safety rules change:

```bash
python3 orchestration/vision/test_screen.py
```

Last re-verified 2026-09-19: all cases pass, including "every real bridge
line must pass the screen" (60/60).

## What actually calls this today

`orchestration/livebridge/middle_audio.py`'s `hint_for()` is the live
entry point (fired from `frigate_events.py`'s `waiting_zone_hint_fn` callback
at PRESHOW start). It imports `generate_middle_openai()`,
`generate_burt_reaction()`, and `generate_outro_lines()` from
`openai_middle.py` — all three reuse this module's `build_system_prompt()`,
`_FULL_FIELDS`, `_norm_images()`, `parse_response()`, and `_screen()`
directly, never reimplementing safety rules per-provider.

## Config knobs (env, `orchestration/livebridge/.env`)

| Var | Default | |
|---|---|---|
| `OPENAI_API_KEY` | — | required for any live call |
| `OPENAI_MODEL` | `gpt-5.6-luna` | confirmed working in a real live call 2026-09-18 (see `openai_middle.py`'s module docstring / `PROJECT_LOG.md` "First-ever real OpenAI success") |
| `FALLBACK_TONE` (constant in code, `middle_pipeline.py`) | `scary` | applied on `uncertain`/timeout |
