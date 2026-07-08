"""Export WordSnap cards to an Anki-importable CSV."""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any

from store import load_clips


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
    parser = argparse.ArgumentParser(
        description="Export WordSnap records to Anki CSV. Pass output.csv to export approved store cards, or input.json output.csv."
    )
    parser.add_argument("paths", type=Path, nargs="+")
    return parser.parse_args()


def records_from_args(paths: list[Path]) -> tuple[list[dict[str, Any]], Path]:
    if len(paths) == 1:
        return [clip for clip in load_clips() if clip.get("status") == "approved" and clip.get("anki")], paths[0]
    if len(paths) == 2:
        return load_records(paths[0]), paths[1]
    raise SystemExit("Usage: export_anki_csv.py [input.json|input.jsonl] output.csv")


def main() -> None:
    args = parse_args()
    records, output_path = records_from_args(args.paths)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with output_path.open("w", newline="", encoding="utf-8-sig") as f:
        writer = csv.DictWriter(f, fieldnames=["Front", "Back", "Tags"])
        writer.writeheader()
        for record in records:
            note = record.get("anki") or {}
            writer.writerow(
                {
                    "Front": note.get("front", record.get("card_front") or record.get("corrected_word", "")),
                    "Back": note.get("back", ""),
                    "Tags": note.get("tags", ""),
                }
            )

    print(f"Wrote {len(records)} cards to {output_path}")


if __name__ == "__main__":
    main()
