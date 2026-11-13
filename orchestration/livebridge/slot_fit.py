"""slot_fit.py -- fit the live clothing insert to the exact sentence it lands in
(2026-09-24, Joe: harden the clothing context to the dialogue; "I don't want to
simply abandon it, I want to be able to dynamically adjust it correctly").

The live insert varies every visit; the 1,200 pre-recorded templates put its
{CLOTHING} slot in ~190 different grammatical contexts. Most accept any noun
phrase, but some don't:

    "That's a wonderful {C}!"         needs ONE item, no article
    "That {C} you're wearing--"       needs ONE item, no article
    "Careful now--{C} is exactly..."  needs a SINGULAR subject
    "That's {C} you've got there."    a single item needs "a"/"an"

Fixed rules, never guesses:
  1. slot_class(a_text, b_text) -> ANY | BARE_SINGULAR | SINGULAR_VERB | NEEDS_ARTICLE,
     read from the text around the slot (MANIFEST.json already stores it).
  2. insert_form(insert) -> the insert's items and whether it's plural.
  3. fit(insert, cls) -> the insert adjusted for that slot (drop/add an
     article, or narrow a list to its first single item), or None if no
     correct form exists for that slot.
The template picker uses fit() to choose a template whose slot the insert
genuinely fits, so the insert is adjusted or placed elsewhere, never dropped.
Locked by orchestration/test_slot_fit.py.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

ANY, BARE_SINGULAR, SINGULAR_VERB, NEEDS_ARTICLE = (
    "ANY", "BARE_SINGULAR", "SINGULAR_VERB", "NEEDS_ARTICLE")

_ARTICLE = re.compile(r"^(a|an|the|some)\s+", re.I)
# Where the main list ends and a trailing detail clause begins ("..., one of
# them dragging a pitchfork", "a suit with a glowing mask").
_CLAUSE = re.compile(r"\s+(?:with|carrying|holding|dragging|clutching|waving)\s+|,\s*one of them\b|,\s*plus\b",
                     re.I)
_ITEM_SPLIT = re.compile(r",\s*(?:and\s+)?|\s+and\s+|\s+plus\s+", re.I)
# Plural-looking words that are one garment, and -s words that are singular.
_PLURAL_GARMENTS = {"jeans", "pants", "overalls", "pajamas", "pyjamas", "glasses",
                    "tights", "shorts", "leggings", "trousers", "sunglasses", "goggles",
                    "boots", "gloves", "shoes", "sneakers", "mittens", "ears", "horns",
                    "wings", "antlers", "fangs", "claws"}
_SINGULAR_S = {"dress", "cross", "mess", "chess", "bus", "octopus", "cactus", "walrus",
               "citrus", "iris", "canvas", "gas", "lens", "series", "species",
               "princess", "goddess", "lioness", "mattress", "harness", "vs"}


def slot_class(a_text: str, b_text: str) -> str:
    a, b = (a_text or "").rstrip(), (b_text or "").lstrip()
    if re.search(r"\b(a|an)(\s+[\w-]+)?$", a, re.I):
        return BARE_SINGULAR                      # "a wonderful {C}", "a {C}"
    if re.search(r"(^|[.!?—-]\s*)That$", a) and re.match(r"you('re| are)\b", b, re.I):
        return BARE_SINGULAR                      # "That {C} you're wearing"
    if re.search(r"\bThat's$", a):
        return NEEDS_ARTICLE                      # "That's {C} you've got there"
    if re.match(r"(is|was|has|does|seems|looks|makes)\b", b, re.I):
        return SINGULAR_VERB                      # "{C} is exactly the kind..."
    return ANY


@dataclass
class InsertForm:
    text: str
    items: list[str]          # top-level items, each with its own article
    plural: bool


def _word_plural(word: str) -> bool:
    w = word.lower().strip(",.")
    if w in _PLURAL_GARMENTS:
        return True
    if w in _SINGULAR_S or w.endswith(("ss", "us", "is")):
        return False
    return w.endswith("s") and len(w) > 2


def insert_form(insert: str) -> InsertForm:
    s = (insert or "").strip()
    main = _CLAUSE.split(s, maxsplit=1)[0].strip()
    items = [i.strip() for i in _ITEM_SPLIT.split(main) if i.strip()]
    if not items:
        return InsertForm(s, [], False)
    plural = len(items) > 1 or _word_plural(items[0].split()[-1])
    return InsertForm(s, items, plural)


def _strip_article(s: str) -> str:
    return _ARTICLE.sub("", s, count=1)


def _with_article(s: str) -> str:
    if _ARTICLE.match(s):
        return s
    return ("an " if re.match(r"[aeiou]", s, re.I) else "a ") + s


def _single(form: InsertForm) -> str | None:
    """The insert as ONE singular item: the whole insert if it already is
    one, else its first item if that item is singular."""
    if not form.items:
        return None
    if not form.plural:
        return form.text
    first = form.items[0]
    return None if _word_plural(first.split()[-1]) else first


def fit(insert: str, cls: str, a_text: str = "") -> str | None:
    """The insert adjusted for this slot, or None if no correct form exists."""
    form = insert_form(insert)
    if not form.items:
        return None
    if cls == ANY:
        return form.text
    if cls == SINGULAR_VERB:
        return _single(form)
    if cls == NEEDS_ARTICLE:
        one = _single(form)
        if form.plural and one is None:
            return form.text                      # "That's matching pirate coats you've got there"
        return _with_article(one or form.text)
    if cls == BARE_SINGULAR:
        one = _single(form)
        if one is None:
            return None
        bare = _strip_article(one)
        m = re.search(r"\b(a|an)$", (a_text or "").rstrip(), re.I)
        if m:                                     # pre-recorded bare "a"/"an" right before the slot
            vowel = bool(re.match(r"[aeiou]", bare, re.I))
            if (m.group(1).lower() == "an") != vowel:
                return None
        return bare
    return None
