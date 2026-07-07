"""Export Wordclip JSON/JSONL card records to an Anki-importable CSV.

Expected input records are the JSON objects printed by process_clip.py --json,
one object per line for JSONL, or a list of objects for JSON.
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any


def load_records(path: Path) -> list[dict[str, Any]]:
    text = path.read_text(encoding="utf-8").strip()
    if not text:
        return []

    if path.suffix.lower() == ".jsonl":
        return [json.loads(line) for line in text.splitlines() if line.strip()]

    data = json.loads(text)
    if isinstance(data, list):
        return data
    return [data]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Export Wordclip records to Anki CSV.")
    parser.add_argument("input", type=Path, help="JSON or JSONL records from process_clip.py")
    parser.add_argument("output", type=Path, help="Destination CSV path")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    records = load_records(args.input)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    with args.output.open("w", newline="", encoding="utf-8-sig") as f:
        writer = csv.DictWriter(f, fieldnames=["Front", "Back", "Tags"])
        writer.writeheader()
        for record in records:
            note = record.get("anki") or {}
            writer.writerow(
                {
                    "Front": note.get("front", record.get("corrected_word", "")),
                    "Back": note.get("back", ""),
                    "Tags": note.get("tags", ""),
                }
            )

    print(f"Wrote {len(records)} cards to {args.output}")


if __name__ == "__main__":
    main()
