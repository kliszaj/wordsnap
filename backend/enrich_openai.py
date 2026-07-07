"""OpenAI side — Whisper transcription + GPT enrichment (openai SDK only)."""
from openai import OpenAI

from schema import Card
from prompt import SYSTEM, user_prompt


def transcribe(wav_path: str, model: str = "whisper-1") -> str:
    """Swedish transcription (not translation). Reads OPENAI_API_KEY from env."""
    client = OpenAI()
    with open(wav_path, "rb") as f:
        r = client.audio.transcriptions.create(
            model=model,
            file=f,
            language="sv",              # transcription in Swedish, per PRD B2
            response_format="json",
        )
    return r.text


def enrich_with_gpt(transcript: str, model: str = "gpt-4o") -> Card:
    """Return a validated Card from a GPT model using OpenAI structured outputs."""
    client = OpenAI()
    completion = client.beta.chat.completions.parse(
        model=model,
        messages=[
            {"role": "system", "content": SYSTEM},
            {"role": "user", "content": user_prompt(transcript)},
        ],
        response_format=Card,
    )
    return completion.choices[0].message.parsed
