# WordSnap - Handoff

Repo: <https://github.com/kliszaj/wordsnap>

Goal: a small ESP32-C6 recorder captures Swedish words/phrases, syncs clips to Hoth/Unraid, transcribes with Whisper, enriches with Claude, and sends clean cards into Anki.

Current deployment target: Hoth at `10.0.0.240`.

---

## Status At A Glance

| Piece | State |
|---|---|
| Device recording | Working on hardware |
| SD organization | Clips live under `/sdcard/clips/`; root clips were auto-migrated |
| Batch sync | Working; tap upload pill to upload all clips, progress bar turns green on clean sync |
| Delete-on-ACK | Working; device deletes local WAV only after server 2xx ACK |
| Empty/invalid WAV cleanup | Added in firmware and backend |
| Burn-in protection | Idle screen sleeps after 30s and wakes on touch |
| Backend on Hoth | Running at `http://10.0.0.240:8090` |
| Web UI | Working: clip list, playback, delete, edit, transcribe, enrich, save to Anki |
| Auto-process on upload | Added: upload ACK is fast, processing runs in the background |
| Phrase/sentence cards | Added: Claude can produce word, phrase, or sentence cards |
| Anki direct save | Working via AnkiConnect |
| Phone Anki loop | Verified: card saved to Anki on Hoth and visible after Anki sync |
| Docker | WordSnap compose/build files exist; Hoth runs Docker Compose |

---

## Verified Live Milestone

Direct Anki integration was proven live on Hoth.

- Anki container UI: `http://10.0.0.240:3010`
- WordSnap UI/API: `http://10.0.0.240:8090`
- WordSnap setting:

  ```text
  ANKI_CONNECT_URL=http://anki:8765
  ANKI_DECK=Swedish::WordSnap
  ```

- The user created deck `Swedish::WordSnap`.
- Saving clip `bd4c12b2aac4` succeeded through AnkiConnect.
- Response showed:

  ```text
  status=exported
  anki_save_mode=ankiconnect
  anki_note_id=1783543047666
  front=tjena
  ```

- User confirmed they saw the card in Anki and then confirmed the full sync worked.

Important: `http://10.0.0.240:3010` is the browser-accessible Anki desktop UI. WordSnap talks to AnkiConnect at `http://anki:8765` inside the Compose network.

---

## Current User-Facing Flow

The old "review / approve" mental model should be considered retired in the UI.

Desired flow now:

```text
record on device
-> tap upload at home
-> backend transcribes/enriches automatically
-> user can play clip and correct transcript/card fields
-> Save to Anki
-> Anki sync makes the card available on phone
```

Visible language should move toward:

- `uploaded`
- `processing`
- `ready`
- `saved`
- `error`

Backend still uses some older internal statuses such as `needs_review`, `approved`, and `exported`. Do not expose those terms prominently in the UI unless/until the backend is cleaned up.

---

## Important Decisions

- Product name: `WordSnap` / UI title `wordclip.`
- Enrichment provider: Claude.
- Claude model default: `claude-sonnet-5`.
- Transcription: OpenAI Whisper API, default `whisper-1`.
- GPT enrichment module is retained for A/B testing but Claude is the chosen path.
- Card style:

  ```text
  Front: Swedish word/phrase/sentence
  Back:
  SWE: kort svensk förklaring.
  ENG: English meaning.
  ```

- Cards may be single words, phrases, or complete sentences.
- Anki integration target is real Anki Desktop in Docker plus AnkiConnect, syncing through AnkiWeb to the phone.
- Current Anki container choice on Hoth: `chrislongros/anki-desktop:latest`, exposed as host port `3010` to container port `3000`.

---

## Hoth / Unraid Compose Shape

The working Hoth stack uses WordSnap plus an Anki desktop container. The Anki UI port had to move off `3000` because Hoth already had that port allocated.

Use this shape:

```yaml
services:
  wordsnap:
    image: kliszaj/wordsnap:latest
    container_name: wordsnap
    restart: unless-stopped
    user: "99:100"
    environment:
      DATA_DIR: /app/data
      ANKI_CONNECT_URL: http://anki:8765
      ANKI_DECK: Swedish::WordSnap
    ports:
      - "8090:8080"
    volumes:
      - /mnt/user/appdata/wordsnap/data:/app/data
    depends_on:
      - anki

  anki:
    image: chrislongros/anki-desktop:latest
    container_name: wordsnap-anki
    restart: unless-stopped
    environment:
      PUID: 99
      PGID: 100
      TZ: Europe/Stockholm
    ports:
      - "3010:3000"
    volumes:
      - /mnt/user/appdata/wordsnap/anki:/config
    security_opt:
      - seccomp=unconfined
    shm_size: 1gb
```

Do not expose AnkiConnect port `8765` to the LAN unless debugging. WordSnap can reach it internally through Docker DNS as `http://anki:8765`.

AnkiConnect setup in Anki:

1. Open `http://10.0.0.240:3010`.
2. Tools -> Add-ons -> Get Add-ons.
3. Install add-on code `2055492159`.
4. Restart Anki.
5. Log into AnkiWeb.
6. Create deck `Swedish::WordSnap`.

---

## Hardware & Firmware

Board: Waveshare ESP32-C6-Touch-LCD-1.83 on COM3 via native USB-Serial-JTAG.

Confirmed pins:

- shared SPI2 bus: SCLK=1, MOSI=2, MISO=16
- LCD: CS=5, DC=3, RST=4, BL=6
- SD: CS=17
- I2S: MCLK=19, BCLK=20, WS=22, DIN=21
- codec I2C: SDA=7, SCL=8
- touch I2C: shared SDA=7, SCL=8; INT=11; reset unmanaged (`-1`)
- BOOT button: GPIO9
- ES7210 mic-array ADC: I2C `0x40`

ESP-IDF v5.5 path:

```text
C:\Users\Adrian\esp\esp-idf
```

Build:

```powershell
cmd /c "call C:\Users\Adrian\esp\esp-idf\export.bat && idf.py -C firmware build"
```

Flash:

```powershell
cmd /c "call C:\Users\Adrian\esp\esp-idf\export.bat && idf.py -C firmware -p COM3 flash"
```

Current firmware behavior:

- Boots, mounts SD, initializes ES7210 audio.
- Stores recordings in `/sdcard/clips/`.
- Auto-migrates legacy root `rec_*.wav` files into `/sdcard/clips/`.
- Home UI has upload pill and red record button.
- Touch red record button to record.
- Tap recording screen to stop.
- BOOT remains a fallback control.
- Max recording duration is capped.
- Upload pill batch-syncs all valid clips.
- Upload progress bar fills during sync and turns green on clean completion.
- Invalid/empty WAVs are skipped/deleted locally.
- Firmware includes capture timestamp when file mtime is valid.
- Upload response can sync device RTC from Hoth server time.

Observed during the last hardware check:

- Device booted cleanly with `sd=1 audio=1 btn=1`.
- Upload tap worked, but the SD card had `0` pending clips at that moment.
- To verify the newest auto-process path on-device, record a fresh clip, then tap upload and watch for `Syncing 1 clip(s)`.

SD gotcha: SD card must be FAT32 + MBR. exFAT caused mount failure earlier.

---

## Backend & Web UI

Backend folder: `backend/`.

Core files:

- `backend/server.py`: FastAPI app, upload/process/save APIs, AnkiConnect calls.
- `backend/store.py`: JSON/file-backed clip store under ignored runtime data.
- `backend/enrich_openai.py`: Whisper transcription and optional GPT path.
- `backend/enrich_claude.py`: Claude structured enrichment.
- `backend/schema.py`: Pydantic card schema.
- `backend/prompt.py`: card prompt for word/phrase/sentence support.
- `backend/anki_format.py`: Anki front/back/tags.
- `backend/settings.py`: persisted settings for API keys, models, and Anki config.
- `backend/web/index.html`, `backend/web/styles.css`, `backend/web/app.js`: web app.

Important endpoints:

- `GET /api/health`
- `GET /api/device/status`
- `POST /api/upload`
- `GET /api/clips`
- `GET /api/clips/{id}/audio`
- `DELETE /api/clips/{id}`
- `POST /api/clips/{id}/process`
- `PATCH /api/clips/{id}`
- `POST /api/clips/{id}/save-to-anki`
- `GET /api/export/anki.csv`
- `GET /api/settings`
- `POST /api/settings`

Current web features:

- Left clip list scrolls independently.
- Delete clips from the side panel.
- Audio playback for selected clip.
- Correct transcript before translation.
- Transcribe button under transcription step.
- Reprocess with Claude under translation step.
- Save to Anki under Anki step.
- Keyboard shortcuts:
  - ArrowUp / ArrowDown navigate clips.
  - Space toggles save/review internally, but this should be revisited now that the UI is Save-to-Anki first.
- Polls while uploaded/processing clips exist.
- Cache-busted assets in `index.html` to avoid stale Hoth browser JS:
  - `styles.css?v=20260708-2`
  - `app.js?v=20260708-2`

Known old Hoth bug and fix:

- Error was: `Could not load clips: can't access property "value", correctedWord is null`.
- Cause: stale/mismatched JS and HTML around `.corrected-word` vs `.card-front`.
- Fix exists locally: cache-busted asset URLs and fallback query selector in `app.js`.
- Make sure the updated image is pushed/pulled on Hoth if this reappears.

---

## Environment

Local ignored env:

```text
backend/.env
```

Expected keys/settings:

```text
OPENAI_API_KEY=
ANTHROPIC_API_KEY=
WHISPER_MODEL=whisper-1
CLAUDE_MODEL=claude-sonnet-5
GPT_MODEL=gpt-5.5
ANKI_CONNECT_URL=http://anki:8765
ANKI_DECK=Swedish::WordSnap
```

Do not print secrets.

Docker build/push:

```powershell
docker compose -f docker-compose.build.yml build
docker push kliszaj/wordsnap:latest
```

Local run:

```powershell
backend\.venv\Scripts\python.exe -m uvicorn server:app --app-dir backend --host 127.0.0.1 --port 8080
```

Local Docker test:

```powershell
docker compose -f docker-compose.build.yml up -d
```

---

## Verification Already Done

- Backend smoke tests with FastAPI TestClient:
  - invalid zero WAV returns 400
  - invalid header returns 400
  - valid WAV upload schedules fake auto-process
  - CSV fallback save marks `approved`
  - fake AnkiConnect save returns note id and marks `exported`
- Python compile check passed for backend modules.
- `node --check backend/web/app.js` passed.
- ESP-IDF firmware build passed.
- Firmware flashed successfully to COM3.
- Local Docker image rebuilt and healthchecked at `127.0.0.1:8080`.
- Local container served cache-busted assets.
- Hoth `/api/health` returned OK.
- Hoth `/api/settings` showed `ANKI_CONNECT_URL=http://anki:8765` and deck `Swedish::WordSnap`.
- Direct save through Hoth returned `200 OK` and created Anki note `1783543047666`.
- User confirmed the Anki card appeared and phone sync worked.

---

## What Is Next

Completed in the follow-up pass after the live Anki test:

- WordSnap now creates a missing Anki deck before saving.
- WordSnap calls AnkiConnect `sync` after a successful note save and records sync metadata.
- The web UI now displays ready/saved/error language instead of the old review/approved wording.
- Settings now shows AnkiConnect status, deck presence, and last Anki save.
- The checked-in Unraid compose includes the Anki desktop container on host port `3010`.

Recommended next order:

1. Push/pull/restart the newest WordSnap Docker image on Hoth after each backend/UI deployment.
2. Record a new device clip and run the full fresh path:
   - record
   - tap upload
   - confirm auto-process
   - save to Anki
   - sync phone
3. Continue device UI design states:
   - idle/ready
   - recording
   - saved
   - syncing
   - sync complete
   - sync error

---

## Git / Ignore Notes

Ignored local/runtime artifacts include:

- `backend/.env`
- `backend/.venv/`
- `backend/data/`
- `backend/*.log`
- `firmware/build/`
- `firmware/managed_components/`

Before this handoff update, the working tree had intentional changes in:

- `README.md`
- `backend/.env.example`
- `backend/server.py`
- `backend/settings.py`
- `backend/web/app.js`
- `backend/web/index.html`
- `docs/product-ui.md`
- `firmware/main/main.c`

The user explicitly asked to save this state to GitHub after the Anki integration worked.
