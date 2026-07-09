"""Formatting helpers for Anki-facing Wordclip cards."""
from __future__ import annotations

from schema import Card


def _clean_sentence(value: str) -> str:
    text = " ".join(value.split())
    if text and text[-1] not in ".!?":
        text += "."
    return text


def front_with_article(front: str, gender: str | None) -> str:
    """Prefix the gender article for nouns, e.g. 'bok' -> 'en bok'.

    `gender` is 'en'/'ett' only for nouns (null otherwise), so this is a no-op for
    non-nouns and for fronts that already start with an article.
    """
    if not front or gender not in ("en", "ett"):
        return front
    if front.split(" ", 1)[0].lower() in ("en", "ett"):
        return front
    return f"{gender} {front}"


def anki_front(card: Card) -> str:
    return front_with_article(card.card_front or card.corrected_word or card.raw_input, card.gender)


def anki_back(card: Card) -> str:
    swedish = _clean_sentence(card.swedish_definition)
    english = _clean_sentence(card.english_definition)
    return f"SWE: {swedish}<br>ENG: {english}"


def anki_tags(iso_week: str) -> str:
    return f"svenska wordclip week::{iso_week}"


def anki_note(card: Card, iso_week: str) -> dict[str, str]:
    return {
        "front": anki_front(card),
        "back": anki_back(card),
        "tags": anki_tags(iso_week),
    }
