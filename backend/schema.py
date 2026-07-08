"""Shared card schema for the enrichment A/B (Claude vs GPT).

The LLM fills the linguistic fields; capture_timestamp / iso_week are derived
from the clip in Python (see compare.py), matching the PRD's separation.
"""
from typing import Literal, Optional, List
from pydantic import BaseModel, ConfigDict, Field


class Card(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    raw_input: str = Field(description="Echo of the transcript text received")
    card_type: Literal["word", "phrase", "sentence"] = Field(
        description="Whether the intended Swedish item is a single word, phrase, or full sentence"
    )
    card_front: str = Field(description="The Swedish item to show on the Anki front; may be a word, phrase, or sentence")
    corrected_word: Optional[str] = Field(
        default=None,
        description="Dictionary form (lemma) for single-word cards; null for phrase or sentence cards unless useful",
    )
    english_definition: str = Field(description="Short English meaning/translation, e.g. 'it depends' or 'receipt'")
    swedish_definition: str = Field(description="Concise definition or explanation written in Swedish")
    part_of_speech: str = Field(description="In Swedish: substantiv, verb, adjektiv, adverb, fras, mening, ...")
    gender: Optional[str] = Field(default=None, description="'en' or 'ett' for nouns; null otherwise")
    forms: Optional[str] = Field(
        default=None,
        description="Nouns: obestämd/bestämd/plural. Verbs: infinitiv/presens/preteritum/supinum. Else null.",
    )
    example_sentence_sv: str = Field(description="Natural example sentence using the word")
    example_sentence_en: str = Field(description="English translation of the example sentence")
    register_: str = Field(
        alias="register",
        description="One of: neutral, formal, informal, slang, literary",
    )
    collocations: List[str] = Field(description="2-4 common collocations/phrases with the word")
    context_note: Optional[str] = Field(
        default=None,
        description="A spoken framing phrase if present (e.g. 'hörde på bussen'); else null",
    )
