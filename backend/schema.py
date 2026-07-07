"""Shared card schema for the enrichment A/B (Claude vs GPT).

The LLM fills the linguistic fields; capture_timestamp / iso_week are derived
from the clip in Python (see compare.py), matching the PRD's separation.
"""
from typing import Optional, List
from pydantic import BaseModel, ConfigDict, Field


class Card(BaseModel):
    model_config = ConfigDict(populate_by_name=True)

    raw_input: str = Field(description="Echo of the transcript text received")
    corrected_word: str = Field(description="Dictionary form (lemma) of the intended Swedish word")
    english_definition: str = Field(description="Short English gloss, e.g. 'accommodating, obliging'")
    swedish_definition: str = Field(description="Concise definition written in Swedish")
    part_of_speech: str = Field(description="In Swedish: substantiv, verb, adjektiv, adverb, ...")
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
