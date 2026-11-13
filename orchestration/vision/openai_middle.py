"""
openai_middle.py -- the ONLY live vision/text backend for the MIDDLE
dialogue pipeline (Gemini support removed 2026-09-18, see
PROJECT_LOG.md "2026-09-18" -- Joe funded a real OpenAI balance and no
longer needed the free-tier fallback). Reuses the EXACT SAME prompt-building
(build_system_prompt), field list (_FULL_FIELDS), character/content rules
(CHARACTER_BIBLE, CONTENT_BOUNDARIES), and safety enforcement
(parse_response -> _screen) that middle_pipeline.py owns -- only the actual
network call lives here. This is deliberate: switching providers, or adding
a second one back later, must never mean switching safety rules, so nothing
content-related is duplicated or re-implemented in this file.

Built 2026-09-18 after Joe funded a real $20 OpenAI balance.

Verified against the ACTUALLY INSTALLED `openai` SDK (3.16.2) at write time --
chat.completions.create, response_format={"type":"json_schema",...}, and the
image_url content-part shape were all read from the real installed package
source, not assumed from memory. OPENAI_MODEL's default ("gpt-5.6-luna") IS
now confirmed working: first real live call 400'd on `temperature=0.7` (this
model tier only supports the default of 1 -- fixed by omitting the param
entirely, see generate_middle_openai()'s comment below), then the very next
call succeeded end-to-end -- real costume-aware line, confirmed via
audio_bridge's own access log that Scarecrow's firmware got a real 200 (not a
404) fetching the resulting MIDDLE file. See PROJECT_LOG.md "2026-09-18",
"First-ever real OpenAI success."

Cost: ~$0.0005-0.0006/call at this prompt/image/output size -- see
docs/_history/vision-cost-and-provider.md's pricing table. Governed by the SAME
GeminiGovernor in middle_audio.py (cooldown, daily cap, lifetime $ cap,
schedule window) for every call -- the governor name
is a holdover from when it also governed Gemini; it's provider-agnostic today.
"""
from __future__ import annotations

import base64
import os

from vision.middle_pipeline import (
    CHARACTER_BIBLE,
    CONTENT_BOUNDARIES,
    THEME_LABEL,
    Character,
    MiddleResult,
    Theme,
    Tone,
    USER_INSTRUCTION,
    _FULL_FIELDS,
    _norm_images,
    _screen,
    build_system_prompt,
    parse_response,
)

# Unverified real API string -- see module docstring. Override with the
# OPENAI_MODEL env var once confirmed against a real OpenAI account/call.
OPENAI_MODEL = os.environ.get("OPENAI_MODEL", "gpt-5.6-luna")
# Completion-token CEILING for the MIDDLE call (reasoning + visible JSON).
# Billed per token actually used, so headroom costs nothing unless needed.
MIDDLE_MAX_COMPLETION_TOKENS = int(os.environ.get("MIDDLE_MAX_COMPLETION_TOKENS", "1200"))

# 2026-10-31 (Joe: "go to the heavy model" on Halloween): real per-call cost
# from the token counts OpenAI returns, so the lifetime $ cap stays honest on
# any model. $/1M tokens (input, output, cached input), post 2026-07-30 cut,
# checked 2026-10-31. Reasoning tokens bill as output. A model NOT in this
# table is refused by middle_audio.active_model() (no guessing on real money).
# Measured 10-31 on a real east snapshot (2,882 in): luna ~$0.0011 (391 out,
# 6.0 s), terra ~$0.0087 (248 out, 4.7 s), sol ~$0.023 (294 out, 3.5 s).
MODEL_PRICES_PER_M = {
    "gpt-5.6-luna": (0.20, 1.20, 0.02),
    "gpt-5.6-terra": (2.00, 12.00, 0.20),
    "gpt-5.6-sol": (5.00, 30.00, 0.50),
}


def cost_usd(model: str, prompt_tokens: int, cached_tokens: int, completion_tokens: int) -> float:
    p_in, p_out, p_cached = MODEL_PRICES_PER_M[model]
    fresh = max(0, prompt_tokens - cached_tokens)
    return (fresh * p_in + cached_tokens * p_cached + completion_tokens * p_out) / 1e6

_TYPE_MAP = {"STRING": "string", "BOOLEAN": "boolean", "INTEGER": "integer"}


def _schema_for_openai(fields) -> dict:
    """JSON-Schema (lowercase types, OpenAI strict-mode shape) built from
    middle_pipeline.py's shared field list (name, type, constraints) --
    OpenAI's casing and strict-mode requirements (additionalProperties:
    false, every property listed in `required`)."""
    props = {}
    for name, typ, extra in fields:
        p = {"type": _TYPE_MAP[typ]}
        if "enum" in extra:
            p["enum"] = extra["enum"]
        if "max_length" in extra:
            p["maxLength"] = extra["max_length"]
        props[name] = p
    order = [n for n, _, _ in fields]
    return {
        "type": "object",
        "properties": props,
        "required": order,
        "additionalProperties": False,
    }


def generate_middle_openai(image, character: Character, theme: Theme,
                           manual_tone: Tone | None = None, *,
                           mime: str = "image/jpeg",
                           model: str | None = None) -> MiddleResult:
    """Enforces the same safety rules via the shared parse_response()/
    _screen() pipeline. LIVE + billed.

    Only sends the FIRST image if given a list -- multi-camera fusion isn't
    implemented here. Not a real gap today: this project only ever uses one
    camera angle in practice (docs/show-trigger-geometry.md §1, "single
    angle, not the E/W pair").
    """
    from openai import OpenAI
    client = OpenAI()  # reads OPENAI_API_KEY from the environment

    imgs = _norm_images(image)
    b64 = base64.b64encode(imgs[0]).decode("ascii")

    system_prompt = build_system_prompt(character, theme, manual_tone)
    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": [
            {"type": "text", "text": USER_INSTRUCTION},
            {"type": "image_url", "image_url": {"url": f"data:{mime};base64,{b64}"}},
        ]},
    ]

    # 2026-09-24 hardening: gpt-5.6-luna is a REASONING model -- it can spend
    # the whole completion budget on invisible reasoning and return empty text
    # (finish_reason="length"). Real show log: 2 of the first 10 live MIDDLE
    # calls (09-21 17:11, 18:36) died as bad_json this way at 400 tokens; the
    # same bug was fixed for the old OUTRO call on 09-22 but never here. The
    # budget is a CEILING (billed per token actually used), so it's set well
    # above need, and an empty/truncated reply gets exactly ONE retry at double.
    # `diag` rides back on the result for the audit log.
    model = model or OPENAI_MODEL
    diag = {"attempts": 0, "finish_reason": None, "reasoning_tokens": None,
            "completion_tokens": None, "model": model, "cost_usd": 0.0}
    text = ""
    budget = MIDDLE_MAX_COMPLETION_TOKENS
    for attempt in (1, 2):
        diag["attempts"] = attempt
        resp = client.chat.completions.create(
            model=model,
            messages=messages,
            response_format={
                "type": "json_schema",
                "json_schema": {
                    "name": "middle_line",
                    "schema": _schema_for_openai(_FULL_FIELDS),
                    "strict": True,
                },
            },
            max_completion_tokens=budget,
            # NOT passing temperature: this model tier 400s on anything but
            # the default (first live call, 2026-09-18).
        )
        choice = resp.choices[0]
        text = choice.message.content or ""
        diag["finish_reason"] = getattr(choice, "finish_reason", None)
        usage = getattr(resp, "usage", None)
        if usage is not None:
            diag["completion_tokens"] = getattr(usage, "completion_tokens", None)
            details = getattr(usage, "completion_tokens_details", None)
            diag["reasoning_tokens"] = getattr(details, "reasoning_tokens", None)
            if model in MODEL_PRICES_PER_M:
                cached = getattr(getattr(usage, "prompt_tokens_details", None), "cached_tokens", 0) or 0
                diag["cost_usd"] += cost_usd(model, usage.prompt_tokens or 0, cached,
                                             usage.completion_tokens or 0)
        if text.strip() and diag["finish_reason"] != "length":
            break
        budget *= 2
    result = parse_response(text, manual_tone=manual_tone)
    result.diag = diag
    return result
