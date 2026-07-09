"""WordSnap Hoth receiver and review web app."""
from __future__ import annotations

import csv
import json
import os
import shutil
import tempfile
from pathlib import Path
from typing import Any
from urllib import request as urlrequest
from urllib.error import URLError

from fastapi import BackgroundTasks, FastAPI, File, Form, HTTPException, Response, UploadFile
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from anki_format import anki_note, front_with_article
from compare import (
    DEFAULT_CLAUDE_MODEL,
    DEFAULT_WHISPER_MODEL,
    derive_clip_metadata,
    load_environment,
    model_to_dict,
)
from enrich_claude import enrich_with_claude
from enrich_openai import transcribe
from store import DATA_DIR, add_clip, delete_clip, find_clip, load_clips, update_clip, utc_now

import settings


app = FastAPI(title="WordSnap Hoth")
WEB_DIR = Path(__file__).with_name("web")
EXPORT_DIR = DATA_DIR / "exports"
MIN_WAV_BYTES = 45

# Bootstrap credentials/config: host .env first (fallback), then appdata settings.json (wins).
load_environment()
settings.apply_to_env()


class ClipPatch(BaseModel):
    transcript: str | None = None
    card_type: str | None = None
    card_front: str | None = None
    corrected_word: str | None = None
    swedish_definition: str | None = None
    english_definition: str | None = None
    status: str | None = None


class ProcessRequest(BaseModel):
    transcript: str | None = None
    card_front: str | None = None
    corrected_word: str | None = None
    iso_week: str | None = None
    capture_timestamp: str | None = None


class SettingsPatch(BaseModel):
    openai_api_key: str | None = None
    anthropic_api_key: str | None = None
    whisper_model: str | None = None
    claude_model: str | None = None
    gpt_model: str | None = None
    anki_connect_url: str | None = None
    anki_deck: str | None = None


def validate_wav_upload(path: Path) -> None:
    size = path.stat().st_size
    if size < MIN_WAV_BYTES:
        raise HTTPException(status_code=400, detail="WAV upload is empty or too short")

    with path.open("rb") as f:
        header = f.read(44)
    if len(header) < 44 or header[0:4] != b"RIFF" or header[8:12] != b"WAVE":
        raise HTTPException(status_code=400, detail="Upload is not a valid WAV file")

    data_size = int.from_bytes(header[40:44], "little")
    if data_size <= 0:
        raise HTTPException(status_code=400, detail="WAV contains no audio samples")


def auto_process_clip(clip_id: str, capture_timestamp: str | None = None) -> None:
    try:
        process_clip(clip_id, ProcessRequest(capture_timestamp=capture_timestamp))
    except HTTPException as exc:
        update_clip(clip_id, {"status": "error", "error": str(exc.detail)})
    except Exception as exc:
        update_clip(clip_id, {"status": "error", "error": f"{type(exc).__name__}: {exc}"})


def _anki_tags(note: dict[str, Any]) -> list[str]:
    tags = note.get("tags", "")
    if isinstance(tags, list):
        return [str(tag) for tag in tags if str(tag).strip()]
    return [tag for tag in str(tags).split() if tag]


def anki_action(action: str, params: dict[str, Any] | None = None, timeout: int = 10) -> Any:
    settings.apply_to_env()
    connect_url = os.getenv("ANKI_CONNECT_URL", "").strip()
    if not connect_url:
        raise RuntimeError("AnkiConnect URL is not configured")

    payload: dict[str, Any] = {"action": action, "version": 6}
    if params is not None:
        payload["params"] = params
    req = urlrequest.Request(
        connect_url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urlrequest.urlopen(req, timeout=timeout) as response:
            result = json.loads(response.read().decode("utf-8"))
    except URLError as exc:
        raise RuntimeError(f"AnkiConnect request failed: {exc}") from exc

    if result.get("error"):
        raise RuntimeError(f"AnkiConnect error: {result['error']}")
    return result.get("result")


def ensure_anki_deck(deck: str) -> None:
    decks = anki_action("deckNames", timeout=5)
    if deck in decks:
        return
    anki_action("createDeck", {"deck": deck}, timeout=10)


def add_note_to_anki(note: dict[str, Any]) -> int:
    settings.apply_to_env()
    deck = os.getenv("ANKI_DECK", "Default").strip() or "Default"
    ensure_anki_deck(deck)
    result = anki_action(
        "addNote",
        {
            "note": {
                "deckName": deck,
                "modelName": "Basic",
                "fields": {
                    "Front": note["front"],
                    "Back": note["back"],
                },
                "tags": _anki_tags(note),
                "options": {"allowDuplicate": False},
            }
        },
    )
    return int(result)


def sync_anki() -> tuple[bool, str | None]:
    try:
        anki_action("sync", timeout=30)
        return True, None
    except Exception as exc:
        return False, str(exc)


def anki_connection_status() -> dict[str, Any]:
    settings.apply_to_env()
    connect_url = os.getenv("ANKI_CONNECT_URL", "").strip()
    deck = os.getenv("ANKI_DECK", "Default").strip() or "Default"
    latest_save = max(
        (clip.get("anki_saved_at", "") for clip in load_clips() if clip.get("anki_saved_at")),
        default="",
    )
    status: dict[str, Any] = {
        "configured": bool(connect_url),
        "connected": False,
        "deck": deck,
        "deck_exists": False,
        "version": None,
        "last_saved_at": latest_save,
        "error": "",
    }
    if not connect_url:
        status["error"] = "AnkiConnect URL is not configured"
        return status
    try:
        status["version"] = anki_action("version", timeout=3)
        status["connected"] = True
        status["deck_exists"] = deck in anki_action("deckNames", timeout=5)
    except Exception as exc:
        status["error"] = str(exc)
    return status


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
        "ready": len([clip for clip in clips if clip["status"] in {"processed", "needs_review"}]),
        "saved": len([clip for clip in clips if clip["status"] in {"approved", "exported"}]),
    }


@app.get("/api/settings")
def get_settings() -> dict[str, Any]:
    """Masked view of configured credentials + effective models (no raw secrets)."""
    data = settings.status()
    data["anki"]["connection"] = anki_connection_status()
    return data


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
        "anki_connect_url": "ANKI_CONNECT_URL",
        "anki_deck": "ANKI_DECK",
    }
    provided = patch.model_dump(exclude_unset=True)
    updates = {env: provided[field] for field, env in field_to_env.items() if field in provided}
    data = settings.update_settings(updates)
    data["anki"]["connection"] = anki_connection_status()
    return data


@app.get("/api/clips")
def clips() -> list[dict[str, Any]]:
    return sorted(load_clips(), key=lambda clip: clip["uploaded_at"], reverse=True)


@app.post("/api/upload")
async def upload_clip(
    background_tasks: BackgroundTasks,
    file: UploadFile = File(...),
    capture_timestamp: str | None = Form(default=None),
    device_id: str | None = Form(default=None),
    auto_process: bool = Form(default=True),
) -> dict[str, Any]:
    suffix = Path(file.filename or "clip.wav").suffix or ".wav"
    with tempfile.NamedTemporaryFile(delete=False, suffix=suffix) as tmp:
        temp_path = Path(tmp.name)
        shutil.copyfileobj(file.file, tmp)
    try:
        validate_wav_upload(temp_path)
        clip = add_clip(temp_path, file.filename or "clip.wav", capture_timestamp=capture_timestamp)
        if device_id:
            clip["device_id"] = device_id
            update_clip(clip["id"], {"device_id": device_id})
        if auto_process:
            background_tasks.add_task(auto_process_clip, clip["id"], capture_timestamp)
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

        requested_front = request.card_front or request.corrected_word
        enrichment_input = transcript
        if requested_front:
            enrichment_input = f"{transcript}\nTarget Swedish item correction from reviewer: {requested_front}"

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
    if {"card_front", "corrected_word", "swedish_definition", "english_definition"} & changes.keys():
        merged = {**clip, **changes}
        front = front_with_article(merged.get("card_front") or merged.get("corrected_word"), merged.get("gender"))
        if front and merged.get("swedish_definition") and merged.get("english_definition"):
            merged_card = {
                "front": front,
                "back": f"SWE: {merged['swedish_definition']}<br>ENG: {merged['english_definition']}",
                "tags": f"svenska wordclip week::{merged.get('iso_week', 'unsorted')}",
            }
            changes["anki"] = merged_card
            changes.setdefault("status", "needs_review")
    return update_clip(clip_id, changes)


@app.post("/api/clips/{clip_id}/approve")
def approve_clip(clip_id: str) -> dict[str, Any]:
    return update_clip(clip_id, {"status": "approved"})


@app.post("/api/clips/{clip_id}/save-to-anki")
def save_clip_to_anki(clip_id: str) -> dict[str, Any]:
    clip = find_clip(clip_id)
    note = clip.get("anki")
    if not note:
        front = front_with_article(clip.get("card_front") or clip.get("corrected_word"), clip.get("gender"))
        if front and clip.get("swedish_definition") and clip.get("english_definition"):
            note = {
                "front": front,
                "back": f"SWE: {clip['swedish_definition']}<br>ENG: {clip['english_definition']}",
                "tags": f"svenska wordclip week::{clip.get('iso_week', 'unsorted')}",
            }
            clip = update_clip(clip_id, {"anki": note})
    if not note:
        raise HTTPException(status_code=400, detail="No Anki note is ready for this clip")

    settings.apply_to_env()
    if not os.getenv("ANKI_CONNECT_URL", "").strip():
        return update_clip(clip_id, {"status": "approved", "anki_save_mode": "csv"})

    try:
        note_id = add_note_to_anki(note)
    except Exception as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    synced, sync_error = sync_anki()
    updates = {
        "status": "exported",
        "anki_save_mode": "ankiconnect",
        "anki_note_id": note_id,
        "anki_saved_at": utc_now(),
    }
    if synced:
        updates["anki_synced_at"] = utc_now()
        updates["anki_sync_error"] = ""
    else:
        updates["anki_sync_error"] = sync_error or "Anki sync failed"
    return update_clip(clip_id, updates)


@app.delete("/api/clips/{clip_id}", status_code=204)
def remove_clip(clip_id: str) -> Response:
    delete_clip(clip_id)
    return Response(status_code=204)


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
