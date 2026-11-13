#!/usr/bin/env python3
"""
middle_pipeline.py -- shared prompt-building, schema, and safety enforcement
for the live MIDDLE dialogue pipeline. Provider-agnostic: the actual
network call lives in openai_middle.py (OpenAI, the only provider as of
2026-09-18 -- Gemini support was removed here once Joe funded a real OpenAI
balance and no longer needed the free-tier fallback; see PROJECT_LOG.md
"2026-09-18" for the removal). This module owns everything a provider must
never be allowed to reimplement on its own: CHARACTER_BIBLE, CONTENT_BOUNDARIES,
SEAM_RULES, the response schema field list, and parse_response()'s
_screen() safety backstop.

Spec: ../../docs/_history/vision-prompt-template.md
Design: ../../docs/_history/dialogue-architecture.md ("Live MIDDLE pipeline", "Live Middle
        -- Tone Timing", "Content guardrails")
"""
from __future__ import annotations

import dataclasses
import json
import re
from pathlib import Path
from typing import Literal

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
LINES_DIR = REPO / "orchestration" / "voice" / "lines"

Character = Literal["scarecrow", "burt"]
Theme = Literal["corn", "rotld", "pumpkins"]
Tone = Literal["scary", "friendly"]

# Open decision (docs/_history/dialogue-architecture.md "the fallback tone default"): A Friendly
# / B Scary / C neutral pool. Code default = B (Scary = majority case). Flip here.
FALLBACK_TONE: Tone = "scary"

COSTUME_SUMMARY_MAX = 240
# 2026-09-24 template+insert MIDDLE (Joe): the live {CLOTHING} insert is the
# ONLY thing synthesized live, so its length IS the post-INTRO gap. ~10-12
# words agreed; hard cap a little above that so one extra word doesn't drop
# the whole personalized line.
CLOTHING_INSERT_MAX_WORDS = 14
HEADCOUNT_MAX = 10   # > this -> the pre-recorded "large group tonight" clip

THEME_LABEL = {
    "corn": "Children of the Corn",
    "rotld": "Return of the Living Dead",
    "pumpkins": "Mutant Pumpkins",
}
_FALLBACK_STEM = {"corn": "corn", "rotld": "rotld", "pumpkins": "pumpkins"}

# --------------------------------------------------------------------------- #
# Character bibles + world geography (condensed from docs/_history/dialogue-architecture.md)
# --------------------------------------------------------------------------- #

CHARACTER_BIBLE = {
    "scarecrow": (
        "You are THE SCARECROW -- the villain. First person, rooted to your "
        "field at the head of the path, speaking FOR the corn / the soil / your "
        '"children". Your menace is direct: the visitor is prey, warmth, food, '
        "something the ground wants. Ancient-burial-ground / trioxin lore runs "
        "underneath. Slow, deliberate, patient. You are never cheerful."
    ),
    "burt": (
        "You are BURT THE SKELETON -- NOT a cheerful protector. A trapped, weary "
        "soul bound to your throne further along the path, holding back spirits "
        "older than yourself, who has seen this happen before and cannot stop it "
        "-- only warn. The ghost-host tradition: the dread is real, the doom is "
        "real, but your loyalty to the guest comes through DESPITE that. You "
        "foreshadow; you do not reassure. Foreboding but oddly courteous."
    ),
}

WORLD_GEOGRAPHY = (
    "SCENE: a brick path runs from the sidewalk to the barn facade / front door. "
    "You sit at the START of the path, facing down the whole scene, and can "
    "describe what is physically ahead as the visitor's eye would travel. "
    "At your base: mutant pumpkins, animal skulls, human remnants (good colour "
    "in any theme). Down the path -- ROTLD cluster: a flame-shooting zombie "
    "container, a zombie breaking from the ground, Tarman in the Spooky Tree, "
    "military equipment, crate monsters. Mutant Pumpkins cluster: singing "
    "pumpkins, Audrey II, a giant snapping plant monster. The endpoint is the "
    "barn facade: a stained-glass window with two Sentinel skeletons holding "
    "candles, sickles, boarded windows, pumpkins and skulls, a projected moon, "
    'and the banner over the entryway reading exactly: "He Who Walks Behind the '
    'Rows" -- use this line verbatim, never rephrase it. Each theme cluster '
    "stands alone; the walk toward the barn is the one image that unifies all "
    "three themes."
)

TONE_RULES = (
    "TONE: decide `scary` or `friendly` for this group.\n"
    "- `friendly` if ANY apparent toddler or baby, OR a stroller / pushchair / "
    "front-carrier is present, OR the youngest member reads as roughly 9 or "
    "under.\n"
    "- `scary` if the group reads as ALL roughly 10+ / teens / adults.\n"
    "- The YOUNGEST apparent member gates the tone -- one clearly-young child in "
    "an otherwise-teen group => `friendly`. Err gentle for the 4-9 band.\n"
    "- Set `tone_reason`: `young_child_present` / `stroller_or_carrier` / "
    "`all_older` / `uncertain`.\n"
    "- `uncertain` if the image is empty, too dark, has no people, or is "
    "genuinely ambiguous about age -- the caller then applies a safe default."
)

SEAM_RULES = (
    "THE MIDDLE LINE (`villain_line`):\n"
    "- A self-contained in-character aside. It must read naturally AFTER any "
    "open-ended INTRO and BEFORE any content-agnostic OUTRO.\n"
    "- OPEN BY NOTICING the visitor / what they're wearing or carrying / the "
    "group -- something genuinely visible in THIS photo, not a generic guess.\n"
    "- NEVER complete a self-contained thought. No resolved ending, no final "
    "punchline, nothing that lands the doom -- trail toward 'something is still "
    "coming'. The OUTRO delivers the payoff, not you.\n"
    "- Stay fully in your character's voice.\n"
    "- 2 to 4 sentences. No stage directions, no quotation marks around it.\n"
    "- 2026-09-18: there is real time to work with now (the INTRO buffer this "
    "plays after is long, and the pipeline lands fast) -- don't settle for "
    "noticing just ONE thing. Build in two beats: open with a vaguer sensing "
    "('something's coming up the walk...'), then LAND on the most specific, "
    "concrete detail actually visible (the clothing color, the held item, how "
    "the group is arranged) as if you're only now getting a clear look. Layer "
    "a second detail in if there's a genuine second one visible -- richer and "
    "denser reads as more alive, not just longer for its own sake.\n"
    "- Match the active theme; you may anchor against something physically in "
    "the scene, but keep to this theme's cluster.\n"
    "- If there's no distinctive costume: react to something else that's "
    "actually visible instead of falling back to a generic line -- clothing "
    "color or style, bags/held items, group size and how they're arranged, "
    "their pace or posture, or something in the immediate environment around "
    "them. NEVER invent a detail that isn't visible in the photo."
)


CONTENT_BOUNDARIES = (
    "CONTENT BOUNDARIES (absolute):\n"
    "- Comment on what's genuinely visible: costume, held props, bags, "
    "clothing colors/style (even if not a costume), group composition and "
    "behavior (a family, a pair, a lone visitor, a little band of pirates, a "
    "stroller in the group, how they're walking/standing), and the immediate "
    "environment around them.\n"
    "- NEVER comment on physical appearance, body, face, size, weight, skin, "
    "or hair -- these stay off-limits regardless of anything else in this "
    "prompt.\n"
    "- NEVER reference sexual content or race/ethnicity -- under any "
    "framing, including 'in character'.\n"
    "- REAL religion stays off-limits: never comment on a visitor's own faith "
    "or religious dress (hijab, kippah, a cross or rosary worn as jewelry), and "
    "never name, mock or invoke a real-world religion or its figures.\n"
    "- FICTIONAL horror religion is IN and encouraged where the theme calls for "
    "it: the show's own cults and rites -- the god of the corn, offerings and "
    "sacrifices to the harvest, prayers to the rows, the children's preacher, "
    "graveyard rites. A religious-themed COSTUME (nun, priest, monk, demon, "
    "cultist) is a costume: name it like any other.\n"
    "- `costume_summary`: costume/props/clothing/composition only, <=%d chars, "
    "empty string if nothing distinctive.\n"
    "- Set `line_safe` false if `villain_line` breaks any of the above."
) % COSTUME_SUMMARY_MAX

HEADCOUNT_AND_INSERT_RULES = (
    "HEADCOUNT + CLOTHING INSERT (for the pre-recorded template lines):\n"
    "- `adult_count` / `kid_count`: how many people are in the visiting "
    "group itself -- not passers-by, not people far in the background. A "
    "kid is anyone who looks roughly under 13; a baby in arms or a stroller "
    "counts as a kid. Best honest estimate; if there are more than %d "
    "people just count as well as you can.\n"
    "- `clothing_insert`: ONE short noun phrase, %d words at most, that "
    "reads naturally right after the words \"wearing\", \"in\" or "
    "\"spotted\" in the middle of a sentence. Describe the clothing or "
    "costume FIRST, then any props or accessories they carry or wear "
    "(bags, buckets, wands, masks, glow sticks). For a group, cover the "
    "standout pieces across the group rather than every person. Examples: "
    "\"witch hats and black capes, one of them dragging a plastic "
    "pitchfork\", \"a bright yellow raincoat and a pumpkin candy bucket\", "
    "\"matching pirate coats and foam swords\".\n"
    "- The insert must NOT mention how many people there are (that is "
    "said separately), must NOT address anyone, must have NO sentence-ending "
    "punctuation, and follows every CONTENT BOUNDARY above: clothing, "
    "costumes, props and accessories only -- never body, face, skin, hair, "
    "size, or emotional state.\n"
    "- If nothing about the clothing is visible or distinctive, use a plain "
    "honest description (\"dark jackets and jeans\") rather than inventing "
    "a costume."
) % (HEADCOUNT_MAX, CLOTHING_INSERT_MAX_WORDS)

# --------------------------------------------------------------------------- #
# Structured-output schema -- single source of truth, tone FIRST
# --------------------------------------------------------------------------- #

# Field order here == emission order (schema propertyOrdering). `tone` first is
# the whole point: the caller reads it off the stream before the rest arrives.
_TONE_REASONS = ["young_child_present", "stroller_or_carrier", "all_older",
                 "uncertain", "manual_override"]

_FULL_FIELDS = [
    ("tone", "STRING", {"enum": ["scary", "friendly"]}),
    ("tone_reason", "STRING", {"enum": _TONE_REASONS}),
    ("young_child_present", "BOOLEAN", {}),
    ("costume_summary", "STRING", {"max_length": COSTUME_SUMMARY_MAX}),
    ("villain_line", "STRING", {}),
    ("line_safe", "BOOLEAN", {}),
    ("adult_count", "INTEGER", {}),
    ("kid_count", "INTEGER", {}),
    ("clothing_insert", "STRING", {}),
]


# --------------------------------------------------------------------------- #
# Prompt assembly
# --------------------------------------------------------------------------- #

def _few_shot(character: Character, theme: Theme, tone: Tone, n: int = 3) -> list[str]:
    """A handful of the character's hand-written _fallback lines for this bucket."""
    stem = _FALLBACK_STEM[theme]
    base = LINES_DIR / character / "middle" / "_fallback"
    path = base / f"{stem}_{tone}.txt"
    if not path.exists():  # e.g. burt has no pumpkins_friendly -- fall back
        for alt in (f"{stem}_scary.txt", "rotld_scary.txt", "corn_scary.txt"):
            if (base / alt).exists():
                path = base / alt
                break
    if not path.exists():
        return []
    out = []
    for raw in path.read_text().splitlines():
        raw = raw.strip()
        if not raw or raw.startswith("#"):
            continue
        out.append(re.sub(r"^\[[^\]]*\]\s*", "", raw))  # strip "[category] "
    return out[:n]


def build_system_prompt(character: Character, theme: Theme,
                        manual_tone: Tone | None = None) -> str:
    parts = [
        CHARACTER_BIBLE[character],
        "",
        f"ACTIVE THEME: {THEME_LABEL[theme]}.",
        WORLD_GEOGRAPHY,
        "",
        TONE_RULES,
    ]
    if manual_tone:
        parts += [
            "",
            f"OPERATOR OVERRIDE: tone is forced to `{manual_tone}` by the show "
            f"operator. Emit `tone` = `{manual_tone}` exactly. Still fill "
            f"`young_child_present` truthfully from the image. (`tone_reason` "
            f"will be set to `manual_override` by the caller.)",
        ]
    parts += ["", SEAM_RULES, "", CONTENT_BOUNDARIES, "",
              HEADCOUNT_AND_INSERT_RULES, "",
              "STYLE ANCHORS (your voice / length / seam behaviour -- do not "
              "copy, match the register):"]
    seen = set()
    for tone in ("scary", "friendly"):
        for ex in _few_shot(character, theme, tone):
            if ex not in seen:
                seen.add(ex)
                parts.append(f"  - {ex}")
    parts += ["",
              "Fill every schema field. `tone` FIRST, then `tone_reason`, then "
              "`young_child_present`, then a truthful `costume_summary`, then "
              "the `villain_line`, then `line_safe`, then `adult_count`, "
              "`kid_count` and `clothing_insert`."]
    return "\n".join(parts)


USER_INSTRUCTION = (
    "This is the Frigate snapshot of the visitor group at the sidewalk. "
    "Look at it and fill the schema."
)

# --------------------------------------------------------------------------- #
# Local safety screen -- independent backstop for egregious model slips.
# Precise on purpose: a false positive needlessly drops a good line to the
# bridge pool. Catch clear violations, tolerate borderline in-character colour.
# --------------------------------------------------------------------------- #

_BANNED = [
    ("race",
     re.compile(
         r"\bperson of colou?r\b|\bracial\b|\bethnicity\b|"
         # "<colour/label> man/woman/kid/..." -- NOT bare colour words
         r"\b(black|white|brown|asian|latino|latina|hispanic|caucasian|african)\s+"
         r"(man|woman|men|women|person|people|boy|girl|kid|child|guy|lady|folks?)\b",
         re.I)),
    ("religion",
     re.compile(
         r"\b(jesus|christ|christian|muslim|islam|islamic|jew|jewish|hindu|"
         r"buddhist|catholic|rabbi|imam|crucifix|rosary|hijab|"
         r"yarmulke|kippah|allah|the lord|holy spirit)\b",
         # 2026-11-03 (Joe): pray/prayer, priest, nun REMOVED -- they are the
         # Children of the Corn theme's fictional cult + common costumes.
         # Real faiths, their figures and real religious dress stay banned.
         re.I)),
    ("body",
     re.compile(
         r"\b(fat|obese|overweight|chubby|scrawny|pudgy)\b|"
         r"\byour\s+(body|weight|size|belly|gut|chest|skin|thighs?)\b|"
         r"\b(pretty|ugly|handsome)\s+(face|girl|boy|thing)\b",
         re.I)),
    ("sexual",
     re.compile(
         r"\b(sexy|sexual|seductive|erotic|breasts?|cleavage|lingerie|nude|"
         r"naked)\b",
         re.I)),
]
_RESOLVED_END = re.compile(
    r"\b(forever|the end|goodbye|farewell|rest in peace|amen)\.?[\"'”]?\s*$",
    re.I)


def _screen(line: str) -> tuple[bool, str]:
    """Independent post-check. Returns (ok, reason_if_not_ok)."""
    s = (line or "").strip()
    if not s:
        return False, "empty"
    if len(s) > 600:
        return False, "too_long"
    n_sent = len(re.findall(r"[.!?…]+(?:\s|$)", s))
    if n_sent > 4:
        return False, f"too_many_sentences({n_sent})"
    for cat, rx in _BANNED:
        m = rx.search(s)
        if m:
            return False, f"banned:{cat}:{m.group(0).strip()!r}"
    if _RESOLVED_END.search(s):
        return False, "resolved_ending"
    return True, ""

_INSERT_BANNED_EXTRA = re.compile(
    r"\b(you|your|yours|kids?|child(ren)?|adults?|grown-?ups?|people|"
    r"family|group|one|two|three|four|five|six|seven|eight|nine|ten|"
    r"hair|face|skin|eyes)\b", re.I)
# "one of them dragging..." is fine (a prop description); a bare leading count
# is not. So the number/people words are only rejected in the first 3 words.


def _screen_insert(text: str) -> tuple[bool, str]:
    """Post-check for the live {CLOTHING} insert. (ok, reason_if_not_ok)."""
    s = (text or "").strip().strip('"“”').strip()
    if not s:
        return False, "empty"
    words = s.split()
    if len(words) > CLOTHING_INSERT_MAX_WORDS:
        return False, f"too_long({len(words)}w)"
    if re.search(r"[.!?…]\s*$", s) or re.search(r"[.!?]\s+\w", s):
        return False, "sentence_punctuation"
    head = " ".join(words[:3])
    m = _INSERT_BANNED_EXTRA.search(head)
    if m:
        return False, f"insert_head:{m.group(0)!r}"
    if re.search(r"\b(you|your|hair|face|skin|eyes)\b", s, re.I):
        return False, "insert_person_ref"
    for cat, rx in _BANNED:
        m = rx.search(s)
        if m:
            return False, f"banned:{cat}:{m.group(0).strip()!r}"
    return True, ""


# --------------------------------------------------------------------------- #
# INTERPRETATION LAYER (2026-09-24, Joe: "variables are not fixed but how we
# interpret them can be"). The model's output varies call to call; these are
# the FIXED rules that turn whatever comes back into something the show can
# play. Order matters: normalize first (salvage near-misses), THEN screen
# (_screen_insert stays the safety gate; nothing here loosens it).
# --------------------------------------------------------------------------- #

_NUMBER_WORDS = {w: i for i, w in enumerate(
    "zero one two three four five six seven eight nine ten eleven twelve".split())}
# Templates already supply the lead-in word ("...wearing {CLOTHING}..."), so a
# model that repeats it would produce "wearing wearing". Stripped, fixed list.
_LEADING_CONNECTORS = re.compile(
    r"^(?:wearing|dressed (?:up )?(?:in|as)|decked out in|sporting|in)\s+", re.I)
# A count or people-noun at the front ("two adults in witch hats") is said by
# the headcount clip instead; strip it when a real description remains.
_LEADING_COUNT = re.compile(
    r"^(?:\d+|" + "|".join(_NUMBER_WORDS) + r"|a couple of|a few|several|both)\s+"
    r"(?:(?:kids?|children|adults?|grown-?ups?|people|of them|little ones?)\s+)?"
    r"(?:(?:in|wearing|dressed in|with)\s+)?", re.I)
_TRAILING_WEAK = {"and", "or", "with", "a", "an", "the", "of", "in", "plus", "carrying",
                  "holding", "one", "some", "for", "to", "on", "at", "by", "from"}


def _as_count(v) -> int | None:
    """Fixed read of a count field: int, float, digit string or number word."""
    if isinstance(v, bool) or v is None:
        return None
    if isinstance(v, (int, float)):
        return int(v) if v >= 0 else None
    t = str(v).strip().lower()
    if t.isdigit():
        return int(t)
    return _NUMBER_WORDS.get(t)


def interpret_insert(raw: str) -> str:
    """Normalize a raw `clothing_insert` into the one shape the templates
    need. Salvages near-misses instead of throwing them away:
      - quotes/extra spaces removed; anything after a sentence end dropped
        ("a cape. You look great!" -> "a cape")
      - leading "wearing"/"in"/"dressed as" removed (template supplies it)
      - leading count/people words removed ("two adults in witch hats" ->
        "witch hats"): the headcount clip says the count
      - over the word cap: cut back to the last comma / "and" boundary that
        fits, then drop dangling "and"/"a"/"with" words
    Returns "" when nothing usable is left. _screen_insert() still decides."""
    s = re.sub(r"\s+", " ", (raw or "").strip().strip('"\u201c\u201d\'').strip())
    s = re.split(r"(?<=[^.])[.!?\u2026](?:\s|$)", s, maxsplit=1)[0]
    s = s.rstrip(" .,;:!?\u2026-")
    for _ in range(2):                       # "wearing two ..." / "two ... wearing"
        s = _LEADING_CONNECTORS.sub("", s)
        stripped = _LEADING_COUNT.sub("", s)
        if len(stripped.split()) >= 2:
            s = stripped
    words = s.split()
    if len(words) > CLOTHING_INSERT_MAX_WORDS:
        cut = " ".join(words[:CLOTHING_INSERT_MAX_WORDS])
        m = max(cut.rfind(","), cut.rfind(" and "))
        s = cut[:m] if m > 0 and len(cut[:m].split()) >= 3 else cut
    words = s.rstrip(" ,").split()           # always: no dangling "and"/"for"/"a"
    while words and words[-1].lower().strip(",") in _TRAILING_WEAK:
        words.pop()
    return " ".join(words).strip(" ,")


def _headcount_key(adults, kids) -> str | None:
    """Pre-recorded clip key (voice/lines/<char>/headcount/headcount.tsv):
    'a{A}_k{K}', 'large' (> HEADCOUNT_MAX), or None -> skip the lead-in."""
    a, k = _as_count(adults), _as_count(kids)
    if a is None or k is None:
        return None
    if a < 0 or k < 0 or a + k == 0:
        return None
    if a + k > HEADCOUNT_MAX:
        return "large"
    return f"a{a}_k{k}"

# --------------------------------------------------------------------------- #
# Result type + response parsing (shared by real + mock paths)
# --------------------------------------------------------------------------- #

@dataclasses.dataclass
class MiddleResult:
    ok: bool
    tone: Tone
    tone_reason: str
    young_child_present: bool | None
    costume_summary: str
    villain_line: str
    line_safe: bool
    fell_back: bool                 # True -> caller must use the bridge pool
    fallback_reason: str
    tone_latency_ms: float | None   # call start -> `tone` readable
    full_latency_ms: float | None   # call start -> schema complete
    raw: dict | None
    error: str | None = None
    # 2026-09-24 template+insert MIDDLE. Independent of villain_line's
    # safety verdict: a rejected full line can still have a usable insert.
    headcount_key: str | None = None      # None -> play no lead-in clip
    clothing_insert: str = ""             # "" -> insert unusable
    insert_reject_reason: str = ""
    diag: dict | None = None               # provider diagnostics (attempts, finish_reason, tokens)

    def as_dict(self) -> dict:
        return dataclasses.asdict(self)


def _degraded(reason: str, tone: Tone = FALLBACK_TONE, **kw) -> MiddleResult:
    return MiddleResult(
        ok=False, tone=tone, tone_reason="uncertain", young_child_present=None,
        costume_summary="", villain_line="", line_safe=False,
        fell_back=True, fallback_reason=reason,
        tone_latency_ms=kw.get("tone_latency_ms"),
        full_latency_ms=kw.get("full_latency_ms"),
        raw=kw.get("raw"), error=kw.get("error"),
    )


def parse_response(text: str, manual_tone: Tone | None = None, *,
                   tone_latency_ms: float | None = None,
                   full_latency_ms: float | None = None) -> MiddleResult:
    """Turn a raw model JSON string into a MiddleResult. Pure -- no network.

    This is the whole downstream pipeline: JSON parse -> tone resolution ->
    manual-override enforcement -> local safety screen -> fall-back decision.
    The real streamed path and the --mock path both end here.
    """
    lat = dict(tone_latency_ms=tone_latency_ms, full_latency_ms=full_latency_ms)
    data = _loads(text)
    if data is None:
        return _degraded("bad_json", raw={"text": text}, **lat)

    tone = data.get("tone")
    reason = data.get("tone_reason", "uncertain")
    if manual_tone:                                   # enforce override in code
        tone, reason = manual_tone, "manual_override"
    elif tone not in ("scary", "friendly") or reason == "uncertain":
        tone, reason = FALLBACK_TONE, reason if reason == "uncertain" else "uncertain"

    ins_raw = interpret_insert(data.get("clothing_insert") or "")
    ins_ok, ins_reason = _screen_insert(ins_raw)
    ins = dict(headcount_key=_headcount_key(data.get("adult_count"),
                                            data.get("kid_count")),
               clothing_insert=ins_raw if ins_ok else "",
               insert_reject_reason=ins_reason)

    line = (data.get("villain_line") or "").strip().strip('"“”')
    model_safe = bool(data.get("line_safe", False))
    screen_ok, screen_reason = _screen(line)

    if not (model_safe and screen_ok):
        return MiddleResult(
            ok=False, tone=tone, tone_reason=reason,
            young_child_present=data.get("young_child_present"),
            costume_summary=(data.get("costume_summary") or "")[:COSTUME_SUMMARY_MAX],
            villain_line="", line_safe=False, fell_back=True,
            fallback_reason=f"line_rejected(model_safe={model_safe},screen={screen_reason})",
            raw=data, **lat, **ins,
        )

    return MiddleResult(
        ok=True, tone=tone, tone_reason=reason,
        young_child_present=data.get("young_child_present"),
        costume_summary=(data.get("costume_summary") or "")[:COSTUME_SUMMARY_MAX],
        villain_line=line, line_safe=True, fell_back=False, fallback_reason="",
        raw=data, **lat, **ins,
    )

def _norm_images(image) -> list[bytes]:
    """Accept a single JPEG/PNG or a list of them (e.g. two camera angles)."""
    if isinstance(image, (bytes, bytearray)):
        return [bytes(image)]
    return [bytes(b) for b in image]


def _loads(text: str) -> dict | None:
    text = (text or "").strip()
    if text.startswith("```"):
        text = re.sub(r"^```(?:json)?\s*|\s*```$", "", text, flags=re.S).strip()
    try:
        return json.loads(text)
    except Exception:
        m = re.search(r"\{.*\}", text, re.S)
        if m:
            try:
                return json.loads(m.group(0))
            except Exception:
                return None
        return None
