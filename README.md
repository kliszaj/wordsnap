# WordSnap

WordSnap is a handheld Swedish vocabulary capture pipeline:

1. ESP32-C6 device records short WAV clips.
2. Hoth/Unraid receiver accepts uploads.
3. OpenAI Whisper transcribes Swedish audio.
4. Claude enriches the target word.
5. The web UI lets you correct, reprocess, approve, and export Anki cards.

## Local Backend Setup

Create `backend/.env` from `backend/.env.example` and fill in:

```text
OPENAI_API_KEY=
ANTHROPIC_API_KEY=
WHISPER_MODEL=whisper-1
CLAUDE_MODEL=claude-sonnet-5
```

Install and run:

```powershell
py -3.12 -m venv backend\.venv
backend\.venv\Scripts\python.exe -m pip install -r backend\requirements.txt
backend\.venv\Scripts\python.exe -m uvicorn server:app --app-dir backend --host 0.0.0.0 --port 8080
```

Open:

```text
http://localhost:8080
```

## Batch Import

Import clips without processing:

```powershell
backend\.venv\Scripts\python.exe backend\process_folder.py test-clips --no-process
```

Import and process:

```powershell
backend\.venv\Scripts\python.exe backend\process_folder.py test-clips --iso-week 2026-W28
```

## Anki Card Shape

Front:

```text
begriplig
```

Back:

```text
SWE: lätt att förstå.
ENG: comprehensible.
```

Export approved cards:

```powershell
backend\.venv\Scripts\python.exe backend\export_anki_csv.py backend\data\exports\anki-approved.csv
```

## Unraid

Use `docker-compose.unraid.yml` as the starting point. Persist `backend/data` to an Unraid appdata share, and keep `backend/.env` private.

```powershell
docker compose -f docker-compose.unraid.yml up -d --build
```

## Device UI

See `firmware/DEVICE_UI.md`. The current firmware has the home/recording UI state machine and BOOT-button start/stop proxy. The next firmware pass is wiring the capacitive touch controller and WiFi upload transport to `POST /api/upload`.
