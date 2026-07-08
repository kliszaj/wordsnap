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
ANKI_CONNECT_URL=
ANKI_DECK=Default
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

If `ANKI_CONNECT_URL` is configured, `Save to Anki` sends directly to that AnkiConnect endpoint and deck.
If it is blank, `Save to Anki` marks the card approved for CSV export.

## Docker Image

Build and tag the Docker Hub image from this repo:

```powershell
docker compose -f docker-compose.build.yml build
```

Push it when you are ready to upload to Docker Hub:

```powershell
docker push kliszaj/wordsnap:latest
```

You can override the image name/tag for testing:

```powershell
$env:WORDSNAP_IMAGE="kliszaj/wordsnap:dev"
docker compose -f docker-compose.build.yml build
docker push $env:WORDSNAP_IMAGE
```

## Hoth / Unraid

`docker-compose.unraid.yml` is the Hoth-ready deploy file. It pulls the Docker Hub image instead of building on the server.

Create `backend/.env` on Hoth with the same values as local setup, then point `WORDSNAP_DATA_DIR` at your appdata share if you do not want to use `./backend/data`.

```powershell
$env:WORDSNAP_IMAGE="kliszaj/wordsnap:latest"
$env:WORDSNAP_DATA_DIR="/mnt/user/appdata/wordsnap/data"
docker compose -f docker-compose.unraid.yml pull
docker compose -f docker-compose.unraid.yml up -d
```

The container serves the review UI on `http://<hoth-ip>:8080` and exposes `/api/health` for compose healthchecks.

## Device UI

See `firmware/DEVICE_UI.md`. The current firmware has the home/recording UI state machine, touch start/stop, and BOOT-button fallback. The next firmware pass is WiFi upload transport to `POST /api/upload`.
