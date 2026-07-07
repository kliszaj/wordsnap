"""Claude enrichment — official Anthropic SDK only (structured output via messages.parse)."""
import anthropic

from schema import Card
from prompt import SYSTEM, user_prompt


def enrich_with_claude(transcript: str, model: str = "claude-sonnet-5") -> Card:
    """Return a validated Card from the Claude model. Reads ANTHROPIC_API_KEY from env."""
    client = anthropic.Anthropic()
    resp = client.messages.parse(
        model=model,
        max_tokens=2048,
        system=SYSTEM,
        messages=[{"role": "user", "content": user_prompt(transcript)}],
        output_format=Card,
    )
    return resp.parsed_output
