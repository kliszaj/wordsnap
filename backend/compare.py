"""Run a Phase 0 enrichment comparison for one recorded Wordclip WAV.

Usage:
    python compare.py path/to/rec_003.wav

The script transcribes Swedish speech with Whisper, sends the same transcript
and prompt to Claude and GPT, then prints the resulting cards side by side.
"""
from __future__ import annotations

import argparse
import os
from datetime import datetime
from pathlib import Path
from typing import Any

from dateutil.parser import isoparse
from dotenv import load_dotenv
from pydantic import BaseModel

from enrich_claude import enrich_with_claude
from enrich_openai import enrich_with_gpt, transcribe


DEFAULT_WHISPER_MODEL = "whisper-1"
DEFAULT_GPT_MODEL = "gpt-5.5"
DEFAULT_CLAUDE_MODEL = "claude-sonnet-5"


def load_environment() -> None:
    """Load backend/.env first, then any caller cwd .env as a fallback."""
    backend_env = Path(__file__).with_name(".env")
    if backend_env.exists():
        load_dotenv(backend_env)
    load_dotenv()


def iso_week_from_datetime(value: datetime) -> str:
    iso_year, iso_week, _ = value.isocalendar()
    return f"{iso_year}-W{iso_week:02d}"


def derive_clip_metadata(
    wav_path: Path,
    capture_timestamp: str | None = None,
    iso_week: str | None = None,
) -> tuple[str, str, bool]:
    if capture_timestamp:
        captured_at = isoparse(capture_timestamp)
        if captured_at.tzinfo is None:
            captured_at = captured_at.astimezone()
        else:
            captured_at = captured_at.astimezone()
        return captured_at.isoformat(timespec="seconds"), iso_week or iso_week_from_datetime(captured_at), False

    modified = datetime.fromtimestamp(wav_path.stat().st_mtime).astimezone()
    suspicious = modified.year < 2020
    return modified.isoformat(timespec="seconds"), iso_week or iso_week_from_datetime(modified), suspicious


def require_env(name: str) -> None:
    if not os.getenv(name):
        raise SystemExit(
            f"Missing {name}. Add it to {Path(__file__).with_name('.env')} "
            "or export it in your shell."
        )


def model_to_dict(model: BaseModel) -> dict[str, Any]:
    return model.model_dump(mode="json", exclude_none=True, by_alias=True)


def print_card(title: str, card: BaseModel, capture_timestamp: str, iso_week: str) -> None:
    data = {
        "capture_timestamp": capture_timestamp,
        "iso_week": iso_week,
        **model_to_dict(card),
    }
    print(f"\n{'=' * 78}")
    print(title)
    print(f"{'=' * 78}")
    for key, value in data.items():
        if isinstance(value, list):
            print(f"{key}:")
            for item in value:
                print(f"  - {item}")
        else:
            print(f"{key}: {value}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare Claude and GPT enrichment for one Wordclip WAV."
    )
    parser.add_argument("wav", type=Path, help="Path to a Swedish word/phrase WAV clip")
    parser.add_argument(
        "--transcript",
        help="Use an existing transcript and skip Whisper transcription.",
    )
    parser.add_argument(
        "--skip-claude",
        action="store_true",
        help="Only run the OpenAI/GPT side.",
    )
    parser.add_argument(
        "--skip-gpt",
        action="store_true",
        help="Only run the Claude side.",
    )
    parser.add_argument(
        "--capture-timestamp",
        help="Override clip timestamp, e.g. 2026-07-07T22:45:00+02:00.",
    )
    parser.add_argument(
        "--iso-week",
        help="Override ISO week metadata, e.g. 2026-W28.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    wav_path = args.wav.expanduser().resolve()
    if not wav_path.exists():
        raise SystemExit(f"WAV file not found: {wav_path}")
    if args.skip_claude and args.skip_gpt:
        raise SystemExit("Nothing to compare: remove one of --skip-claude or --skip-gpt.")

    load_environment()

    whisper_model = os.getenv("WHISPER_MODEL", DEFAULT_WHISPER_MODEL)
    gpt_model = os.getenv("GPT_MODEL", DEFAULT_GPT_MODEL)
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

    print("\nTranscript")
    print("-" * 78)
    print(transcript)
    print(f"\nClip metadata: capture_timestamp={capture_timestamp}, iso_week={iso_week}")
    if suspicious_timestamp:
        print(
            "Note: clip mtime looks like an unset device clock. Use "
            "--capture-timestamp or --iso-week to override metadata."
        )

    if not args.skip_claude:
        require_env("ANTHROPIC_API_KEY")
        print(f"\nRunning Claude enrichment with {claude_model}...")
        claude_card = enrich_with_claude(transcript, model=claude_model)
        print_card(f"Claude ({claude_model})", claude_card, capture_timestamp, iso_week)

    if not args.skip_gpt:
        require_env("OPENAI_API_KEY")
        print(f"\nRunning GPT enrichment with {gpt_model}...")
        gpt_card = enrich_with_gpt(transcript, model=gpt_model)
        print_card(f"GPT ({gpt_model})", gpt_card, capture_timestamp, iso_week)


if __name__ == "__main__":
    main()
