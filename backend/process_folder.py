"""Import and process a folder of WordSnap WAV clips into the review store."""
from __future__ import annotations

import argparse
from pathlib import Path

from server import ProcessRequest, process_clip
from store import import_folder


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Import/process a folder of rec_*.wav files.")
    parser.add_argument("folder", type=Path, help="Folder containing WAV clips")
    parser.add_argument("--pattern", default="rec_*.wav", help="Glob pattern, default rec_*.wav")
    parser.add_argument("--iso-week", help="Override ISO week for imported clips, e.g. 2026-W28")
    parser.add_argument(
        "--no-process",
        action="store_true",
        help="Only import clips into the review store; do not call Whisper/Claude.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not args.folder.exists():
        raise SystemExit(f"Folder not found: {args.folder}")

    clips = import_folder(args.folder, pattern=args.pattern)
    print(f"Imported {len(clips)} clips")
    if args.no_process:
        return

    for clip in clips:
        print(f"Processing {clip['original_name']} ({clip['id']})")
        process_clip(clip["id"], ProcessRequest(iso_week=args.iso_week))


if __name__ == "__main__":
    main()
