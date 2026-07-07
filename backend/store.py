"""Tiny file-backed store for the WordSnap Hoth receiver."""
from __future__ import annotations

import json
import shutil
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


DATA_DIR = Path(__file__).with_name("data")
CLIP_DIR = DATA_DIR / "clips"
DB_PATH = DATA_DIR / "clips.json"


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def ensure_store() -> None:
    CLIP_DIR.mkdir(parents=True, exist_ok=True)
    if not DB_PATH.exists():
        DB_PATH.write_text("[]\n", encoding="utf-8")


def load_clips() -> list[dict[str, Any]]:
    ensure_store()
    return json.loads(DB_PATH.read_text(encoding="utf-8"))


def save_clips(clips: list[dict[str, Any]]) -> None:
    ensure_store()
    tmp = DB_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(clips, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    tmp.replace(DB_PATH)


def find_clip(clip_id: str) -> dict[str, Any]:
    for clip in load_clips():
        if clip["id"] == clip_id:
            return clip
    raise KeyError(clip_id)


def update_clip(clip_id: str, changes: dict[str, Any]) -> dict[str, Any]:
    clips = load_clips()
    for clip in clips:
        if clip["id"] == clip_id:
            clip.update(changes)
            clip["updated_at"] = utc_now()
            save_clips(clips)
            return clip
    raise KeyError(clip_id)


def add_clip(source_path: Path, original_name: str, capture_timestamp: str | None = None) -> dict[str, Any]:
    ensure_store()
    clip_id = uuid.uuid4().hex[:12]
    safe_name = Path(original_name).name or f"{clip_id}.wav"
    stored_name = f"{clip_id}-{safe_name}"
    stored_path = CLIP_DIR / stored_name
    shutil.copyfile(source_path, stored_path)

    now = utc_now()
    clip = {
        "id": clip_id,
        "status": "uploaded",
        "original_name": safe_name,
        "stored_path": str(stored_path),
        "capture_timestamp": capture_timestamp,
        "uploaded_at": now,
        "updated_at": now,
        "transcript": "",
        "corrected_word": "",
        "swedish_definition": "",
        "english_definition": "",
        "anki": None,
        "error": "",
    }
    clips = load_clips()
    clips.append(clip)
    save_clips(clips)
    return clip


def import_folder(folder: Path, pattern: str = "rec_*.wav") -> list[dict[str, Any]]:
    added = []
    for wav in sorted(folder.glob(pattern)):
        added.append(add_clip(wav, wav.name))
    return added
