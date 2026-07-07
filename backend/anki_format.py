"""Formatting helpers for Anki-facing Wordclip cards."""
from __future__ import annotations

from schema import Card


def _clean_sentence(value: str) -> str:
    text = " ".join(value.split())
    if text and text[-1] not in ".!?":
        text += "."
    return text


def anki_front(card: Card) -> str:
    return card.corrected_word


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
