"""Process one Wordclip WAV with the selected Phase 0 pipeline.

Pipeline:
    WAV -> Whisper transcription -> Claude enrichment -> printable card record
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

from anki_format import anki_note
from compare import (
    DEFAULT_CLAUDE_MODEL,
    DEFAULT_WHISPER_MODEL,
    derive_clip_metadata,
    load_environment,
    model_to_dict,
    print_card,
    require_env,
)
from enrich_claude import enrich_with_claude
from enrich_openai import transcribe


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Process one Wordclip WAV using Claude enrichment.")
    parser.add_argument("wav", type=Path, help="Path to a Swedish word/phrase WAV clip")
    parser.add_argument(
        "--transcript",
        help="Use an existing transcript and skip Whisper transcription.",
    )
    parser.add_argument(
        "--capture-timestamp",
        help="Override clip timestamp, e.g. 2026-07-07T22:45:00+02:00.",
    )
    parser.add_argument(
        "--iso-week",
        help="Override ISO week metadata, e.g. 2026-W28.",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print the enriched card as JSON instead of a readable text card.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    wav_path = args.wav.expanduser().resolve()
    if not wav_path.exists():
        raise SystemExit(f"WAV file not found: {wav_path}")

    load_environment()
    require_env("ANTHROPIC_API_KEY")

    whisper_model = os.getenv("WHISPER_MODEL", DEFAULT_WHISPER_MODEL)
    claude_model = os.getenv("CLAUDE_MODEL", DEFAULT_CLAUDE_MODEL)

    capture_timestamp, iso_week, suspicious_timestamp = derive_clip_metadata(
        wav_path,
        capture_timestamp=args.capture_timestamp,
        iso_week=args.iso_week,
    )

    transcript = args.transcript
    if transcript is None:
        require_env("OPENAI_API_KEY")
        print(f"Transcribing with {whisper_model}: {wav_path}")
        transcript = transcribe(str(wav_path), model=whisper_model)

    print(f"Running Claude enrichment with {claude_model}...")
    card = enrich_with_claude(transcript, model=claude_model)
    if card is None:
        raise SystemExit("Claude returned no parsed card.")

    record = {
        "capture_timestamp": capture_timestamp,
        "iso_week": iso_week,
        "source_file": str(wav_path),
        "transcript": transcript,
        "anki": anki_note(card, iso_week),
        **model_to_dict(card),
    }

    if args.json:
        print(json.dumps(record, ensure_ascii=False, indent=2))
    else:
        print("\nTranscript")
        print("-" * 78)
        print(transcript)
        if suspicious_timestamp:
            print(
                "\nNote: clip mtime looks like an unset device clock. Use "
                "--capture-timestamp or --iso-week to override metadata."
            )
        note = anki_note(card, iso_week)
        print("\nAnki")
        print("-" * 78)
        print(f"Front: {note['front']}")
        print(f"Back: {note['back'].replace('<br>', chr(10))}")
        print(f"Tags: {note['tags']}")
        print_card(f"Claude ({claude_model})", card, capture_timestamp, iso_week)


if __name__ == "__main__":
    main()
