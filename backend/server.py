"""WordSnap Hoth receiver and review web app."""
from __future__ import annotations

import csv
import os
import shutil
import tempfile
from pathlib import Path
from typing import Any

from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from anki_format import anki_note
from compare import (
    DEFAULT_CLAUDE_MODEL,
    DEFAULT_WHISPER_MODEL,
    derive_clip_metadata,
    load_environment,
    model_to_dict,
)
from enrich_claude import enrich_with_claude
from enrich_openai import transcribe
from store import DATA_DIR, add_clip, find_clip, load_clips, update_clip, utc_now

import settings


app = FastAPI(title="WordSnap Hoth")
WEB_DIR = Path(__file__).with_name("web")
EXPORT_DIR = DATA_DIR / "exports"

# Bootstrap credentials/config: host .env first (fallback), then appdata settings.json (wins).
load_environment()
settings.apply_to_env()


class ClipPatch(BaseModel):
    transcript: str | None = None
    corrected_word: str | None = None
    swedish_definition: str | None = None
    english_definition: str | None = None
    status: str | None = None


class ProcessRequest(BaseModel):
    transcript: str | None = None
    corrected_word: str | None = None
    iso_week: str | None = None
    capture_timestamp: str | None = None


class SettingsPatch(BaseModel):
    openai_api_key: str | None = None
    anthropic_api_key: str | None = None
    whisper_model: str | None = None
    claude_model: str | None = None
    gpt_model: str | None = None


if WEB_DIR.exists():
    app.mount("/assets", StaticFiles(directory=WEB_DIR), name="assets")


@app.get("/", response_class=HTMLResponse)
def index() -> str:
    return (WEB_DIR / "index.html").read_text(encoding="utf-8")


@app.get("/api/health")
def health() -> dict[str, str]:
    return {"status": "ok"}


@app.get("/api/device/status")
def device_status() -> dict[str, Any]:
    clips = load_clips()
    return {
        "server_time": utc_now(),
        "uploaded_clips": len(clips),
        "needs_review": len([clip for clip in clips if clip["status"] in {"processed", "needs_review"}]),
        "approved": len([clip for clip in clips if clip["status"] == "approved"]),
    }


@app.get("/api/settings")
def get_settings() -> dict[str, Any]:
    """Masked view of configured credentials + effective models (no raw secrets)."""
    return settings.status()


@app.post("/api/settings")
def save_settings(patch: SettingsPatch) -> dict[str, Any]:
    """Persist provided keys/models to appdata and apply them to the running process.

    Only fields present in the request are changed; send "" to clear a field.
    """
    field_to_env = {
        "openai_api_key": "OPENAI_API_KEY",
        "anthropic_api_key": "ANTHROPIC_API_KEY",
        "whisper_model": "WHISPER_MODEL",
        "claude_model": "CLAUDE_MODEL",
        "gpt_model": "GPT_MODEL",
    }
    provided = patch.model_dump(exclude_unset=True)
    updates = {env: provided[field] for field, env in field_to_env.items() if field in provided}
    return settings.update_settings(updates)


@app.get("/api/clips")
def clips() -> list[dict[str, Any]]:
    return sorted(load_clips(), key=lambda clip: clip["uploaded_at"], reverse=True)


@app.post("/api/upload")
async def upload_clip(
    file: UploadFile = File(...),
    capture_timestamp: str | None = Form(default=None),
    device_id: str | None = Form(default=None),
) -> dict[str, Any]:
    suffix = Path(file.filename or "clip.wav").suffix or ".wav"
    with tempfile.NamedTemporaryFile(delete=False, suffix=suffix) as tmp:
        temp_path = Path(tmp.name)
        shutil.copyfileobj(file.file, tmp)
    try:
        clip = add_clip(temp_path, file.filename or "clip.wav", capture_timestamp=capture_timestamp)
        if device_id:
            clip["device_id"] = device_id
            update_clip(clip["id"], {"device_id": device_id})
        return {"id": clip["id"], "status": clip["status"], "server_time": utc_now()}
    finally:
        temp_path.unlink(missing_ok=True)


@app.post("/api/clips/{clip_id}/process")
def process_clip(clip_id: str, request: ProcessRequest | None = None) -> dict[str, Any]:
    request = request or ProcessRequest()
    load_environment()
    settings.apply_to_env()
    if not os.getenv("ANTHROPIC_API_KEY"):
        raise HTTPException(status_code=400, detail="Anthropic API key not configured. Add it in Settings.")

    clip = find_clip(clip_id)
    wav_path = Path(clip["stored_path"])
    if not wav_path.exists():
        raise HTTPException(status_code=404, detail="Stored WAV is missing")

    update_clip(clip_id, {"status": "processing", "error": ""})
    try:
        transcript = request.transcript or clip.get("transcript") or ""
        if not transcript:
            if not os.getenv("OPENAI_API_KEY"):
                raise HTTPException(status_code=400, detail="OpenAI API key not configured. Add it in Settings.")
            transcript = transcribe(str(wav_path), model=os.getenv("WHISPER_MODEL", DEFAULT_WHISPER_MODEL))

        enrichment_input = transcript
        if request.corrected_word:
            enrichment_input = f"{transcript}\nTarget word correction from reviewer: {request.corrected_word}"

        card = enrich_with_claude(enrichment_input, model=os.getenv("CLAUDE_MODEL", DEFAULT_CLAUDE_MODEL))
        if card is None:
            raise RuntimeError("Claude returned no parsed card")

        capture_timestamp, iso_week, _ = derive_clip_metadata(
            wav_path,
            capture_timestamp=request.capture_timestamp or clip.get("capture_timestamp"),
            iso_week=request.iso_week,
        )
        note = anki_note(card, iso_week)
        record = {
            "status": "processed",
            "transcript": transcript,
            "capture_timestamp": capture_timestamp,
            "iso_week": iso_week,
            "anki": note,
            **model_to_dict(card),
            "error": "",
        }
        return update_clip(clip_id, record)
    except Exception as exc:
        update_clip(clip_id, {"status": "error", "error": f"{type(exc).__name__}: {exc}"})
        raise


@app.patch("/api/clips/{clip_id}")
def patch_clip(clip_id: str, patch: ClipPatch) -> dict[str, Any]:
    changes = {k: v for k, v in patch.model_dump().items() if v is not None}
    clip = find_clip(clip_id)
    if {"corrected_word", "swedish_definition", "english_definition"} & changes.keys():
        merged = {**clip, **changes}
        if merged.get("corrected_word") and merged.get("swedish_definition") and merged.get("english_definition"):
            merged_card = {
                "front": merged["corrected_word"],
                "back": f"SWE: {merged['swedish_definition']}<br>ENG: {merged['english_definition']}",
                "tags": f"svenska wordclip week::{merged.get('iso_week', 'unsorted')}",
            }
            changes["anki"] = merged_card
            changes.setdefault("status", "needs_review")
    return update_clip(clip_id, changes)


@app.post("/api/clips/{clip_id}/approve")
def approve_clip(clip_id: str) -> dict[str, Any]:
    return update_clip(clip_id, {"status": "approved"})


@app.get("/api/clips/{clip_id}/audio")
def clip_audio(clip_id: str) -> FileResponse:
    clip = find_clip(clip_id)
    return FileResponse(Path(clip["stored_path"]), media_type="audio/wav", filename=clip["original_name"])


@app.get("/api/export/anki.csv")
def export_anki_csv() -> FileResponse:
    EXPORT_DIR.mkdir(parents=True, exist_ok=True)
    path = EXPORT_DIR / "anki-approved.csv"
    approved = [clip for clip in load_clips() if clip.get("status") == "approved" and clip.get("anki")]
    with path.open("w", newline="", encoding="utf-8-sig") as f:
        writer = csv.DictWriter(f, fieldnames=["Front", "Back", "Tags"])
        writer.writeheader()
        for clip in approved:
            note = clip["anki"]
            writer.writerow({"Front": note["front"], "Back": note["back"], "Tags": note["tags"]})
    return FileResponse(path, media_type="text/csv", filename="wordsnap-anki-approved.csv")
