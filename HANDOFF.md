# WordSnap — Handoff

Context for picking up this project. Goal (per [prd-wordclip.md](prd-wordclip.md)): a handheld
ESP32-C6 device captures spoken Swedish words, uploads clips to a self-hosted Hoth/Unraid pipeline,
transcribes with Whisper, enriches with Claude, and produces reviewable Anki cards sorted by ISO week.

Latest pushed commit at handoff time:

```text
f63893c Compact pipeline cards
```

Repo: <https://github.com/kliszaj/wordsnap>

---

## Status at a glance

| Piece | State |
|---|---|
| Device audio capture to SD | Working and previously verified on hardware |
| Device visual UI scaffold | Added and builds; touchscreen starts/stops recording; BOOT remains fallback |
| Touchscreen input | Wired for CST816 touch hit-testing on record/upload targets |
| Device WiFi upload | API contract and firmware stub exist; transport not implemented |
| Backend Claude pipeline | Working; Claude selected as enrichment provider |
| Hoth receiver API | Implemented with FastAPI and local JSON/file store |
| Web review UI | Implemented dark dashboard for upload, playback, edit, reprocess, approve, export |
| Docker / Hoth deploy | Docker Hub image + compose deploy path added and build-verified |
| Anki export | CSV export implemented for approved cards |
| Direct AnkiConnect | Not implemented yet |

---

## Important decisions made

- **Enrichment provider:** Claude, model env default `CLAUDE_MODEL=claude-sonnet-5`.
- **Transcription:** OpenAI Whisper API, default `WHISPER_MODEL=whisper-1`.
- **Card style:** Anki front is the Swedish word. Back is compact:

  ```text
  SWE: lätt att förstå.
  ENG: comprehensible.
  ```

- **Review-first workflow:** uploaded clips should be reviewed/corrected in the web UI before Anki export.
- **Device/website visual language:** dark charcoal, red recording accent, rounded pills, large tactile controls. See [docs/product-ui.md](docs/product-ui.md).
- **Current website direction:** product title is `wordclip.`, using the review-ledger layout plus paper-tape pipeline cards and the subtler dot-grid palette.
- **Toolbar decision:** remove app-level `Refresh` because browser refresh is enough; remove `UI directions` from the production toolbar; keep `Upload WAV` and `Export CSV`.
- **Anki export decision:** CSV is acceptable for simple note import. `.apkg` is the packaged deck format, useful later if WordClip needs to bundle note types/media/scheduling.

---

## Hardware & environment

Board: **Waveshare ESP32-C6-Touch-LCD-1.83** on **COM3** via native USB-Serial-JTAG.
LCD: 240×284 ST7789.
Touch: CST816D/CST816S-compatible capacitive controller over I2C.

Confirmed pins used so far:

- shared SPI2 bus: SCLK=1, MOSI=2, MISO=16
- LCD: CS=5, DC=3, RST=4, BL=6
- SD: CS=17
- I2S: MCLK=19, BCLK=20, WS=22, DIN=21
- codec I2C: SDA=7, SCL=8
- touch I2C: shared SDA=7, SCL=8; INT=11; reset left unmanaged (`-1`) per Waveshare ESP-IDF example because GPIO4 is already LCD reset
- BOOT button: GPIO9
- ES7210 mic-array ADC: I2C `0x40`

ESP-IDF v5.5 is installed at:

```text
C:\Users\Adrian\esp\esp-idf
```

Build command:

```powershell
cmd /c "call C:\Users\Adrian\esp\esp-idf\export.bat && idf.py -C firmware build"
```

Flash command:

```powershell
cmd /c "call C:\Users\Adrian\esp\esp-idf\export.bat && idf.py -C firmware -p COM3 flash"
```

Monitor gotcha: `idf.py monitor` has failed in this shell before. For reset/status, use esptool reset
or a simple pyserial reader after USB settles. Firmware logs a heartbeat.

SD gotcha: SD card must be **FAT32 + MBR**. exFAT silently caused mount failure earlier.

---

## Firmware state

Main file: [firmware/main/main.c](firmware/main/main.c)

Current behavior:

- Boots, mounts SD, initializes ES7210 audio.
- Initializes CST816 touch on the shared I2C bus.
- Draws WordSnap-style home screen:
  - upload pill shape
  - large red record circle
- Tap the red record circle, or press BOOT, to start recording.
- Tap the recording screen, or release then press BOOT again, to stop early.
- Safety cap is `MAX_RECORD_SECONDS=15`.
- WAV header is rewritten after stop so short clips are valid.
- Recording view draws:
  - dark dotted background
  - red REC dot
  - clip-index pill placeholder
- Upload touch target and upload transport are stubbed:
  - upload pill hit-testing exists and calls `upload_pending_clips()`
  - `upload_pending_clips()`

Firmware build passed after wiring CST816 touch:

```powershell
cmd /c "call C:\Users\Adrian\esp\esp-idf\export.bat && idf.py -C firmware build"
```

Notes for next firmware agent:

1. Flash and hardware-test touch coordinates/orientation:
   - home record circle should start capture
   - recording screen tap should stop after the 600ms guard
   - upload pill should enter `upload_pending_clips()`
   - logs print `touch tap x=... y=...`
2. If coordinates are rotated/mirrored, adjust `.swap_xy`, `.mirror_x`, or `.mirror_y` in `touch_init()`.
3. Add text rendering or LVGL for:
   - `Upload [12]`
   - `00:12`
   - `REC`
   - clip number pill
4. Implement WiFi upload:
   - `POST /api/upload`
   - multipart field `file`
   - optional `capture_timestamp`
   - optional `device_id`
   - delete local WAV only after server ACK.
5. Keep WiFi upload and I2S capture as separate modes.

See [firmware/DEVICE_UI.md](firmware/DEVICE_UI.md).

---

## Backend state

Backend folder: [backend/](backend/)

Core pipeline files:

- [backend/enrich_openai.py](backend/enrich_openai.py): Whisper transcription and optional GPT lab enrichment.
- [backend/enrich_claude.py](backend/enrich_claude.py): Claude structured-output enrichment.
- [backend/schema.py](backend/schema.py): Pydantic `Card`.
- [backend/prompt.py](backend/prompt.py): prompt tuned for compact `SWE`/`ENG` Anki back.
- [backend/anki_format.py](backend/anki_format.py): creates `front`, `back`, `tags`.
- [backend/process_clip.py](backend/process_clip.py): one-clip Claude path.
- [backend/compare.py](backend/compare.py): retained as Claude-vs-GPT lab tool.

Server/review UI files:

- [backend/server.py](backend/server.py): FastAPI app.
- [backend/store.py](backend/store.py): file-backed JSON store under ignored `backend/data/`.
- [backend/web/index.html](backend/web/index.html)
- [backend/web/styles.css](backend/web/styles.css)
- [backend/web/app.js](backend/web/app.js)
- [backend/process_folder.py](backend/process_folder.py): import/process a folder of WAV clips.
- [backend/export_anki_csv.py](backend/export_anki_csv.py): export approved cards.
- [backend/Dockerfile](backend/Dockerfile)
- [backend/.dockerignore](backend/.dockerignore): keeps `.env`, runtime data, logs, caches, and venv out of images.
- [docker-compose.build.yml](docker-compose.build.yml): local image build/tag for Docker Hub.
- [docker-compose.unraid.yml](docker-compose.unraid.yml): Hoth/Unraid deploy file that pulls `kliszaj/wordsnap:latest`.

Env:

```text
backend/.env
```

is local/ignored and should contain:

```text
OPENAI_API_KEY=
ANTHROPIC_API_KEY=
WHISPER_MODEL=whisper-1
CLAUDE_MODEL=claude-sonnet-5
GPT_MODEL=gpt-5.5
```

Do not print secrets.

Docker build/push/deploy:

```powershell
docker compose -f docker-compose.build.yml build
docker push kliszaj/wordsnap:latest
docker compose -f docker-compose.unraid.yml pull
docker compose -f docker-compose.unraid.yml up -d
```

Notes:

- `docker-compose.unraid.yml` exposes `8080`, reads `backend/.env`, persists `/app/data`, and healthchecks `/api/health`.
- Set `WORDSNAP_DATA_DIR=/mnt/user/appdata/wordsnap/data` on Hoth to persist data in Unraid appdata.
- Set `WORDSNAP_IMAGE=kliszaj/wordsnap:<tag>` to deploy a non-latest tag.

Run local server:

```powershell
backend\.venv\Scripts\python.exe -m uvicorn server:app --app-dir backend --host 127.0.0.1 --port 8080
```

Open:

```text
http://127.0.0.1:8080
```

The web UI supports:

- uploading WAVs
- seeing clip inbox/status
- audio playback
- editing transcript, corrected word, SWE, ENG
- processing/reprocessing with Claude
- approving cards
- exporting approved Anki CSV at `/api/export/anki.csv`

Latest local UI work since the last push:

- Header title changed from `Review ledger` to `wordclip.`
- Pipeline cards were compacted to remove excessive empty vertical space.
- Pipeline card punch holes/subheaders were removed.
- The Anki preview card was restyled to match the Transcription and Translation cards:
  - `Card front` field
  - `Card back` field
  - same compact field treatment
  - all three cards verified at `319px` height in browser DOM.
- Toolbar reduced to:
  - `Upload WAV`
  - `Export CSV`
- `Refresh` and `UI directions` were removed from the toolbar, and the unused refresh JS listener was removed.
- `Upload WAV` was changed to use the same accent button styling family as `Export CSV`.

API endpoints:

- `GET /api/device/status`
- `POST /api/upload`
- `GET /api/clips`
- `GET /api/clips/{id}/audio`
- `POST /api/clips/{id}/process`
- `PATCH /api/clips/{id}`
- `POST /api/clips/{id}/approve`
- `GET /api/export/anki.csv`

Verification already done:

- backend dependencies installed into `backend/.venv`
- Python `py_compile` passed for backend modules
- FastAPI `GET /` and `GET /api/device/status` smoke tests passed
- `POST /api/upload` accepted a local WAV
- ESP-IDF firmware build passed
- Latest local web smoke check: `GET http://127.0.0.1:8080/` returned `200`.
- Browser DOM check after toolbar cleanup showed only `Upload WAV` and `Export CSV` in `.toolbar-actions`.

Note: a browser crop screenshot attempt was visually offset/unhelpful, but DOM checks confirmed the important
layout metrics. Full-page browser preview itself was reachable at `http://127.0.0.1:8080`.

---

## Recorded/eval clips

Existing sample/test clips:

- [test-clips/rec_003.wav](test-clips/rec_003.wav)
- [test-clips/rec_005.wav](test-clips/rec_005.wav) through [test-clips/rec_014.wav](test-clips/rec_014.wav)

The 10-clip A/B test order was:

1. en skjorta
2. ett kvitto
3. att skynda
4. att låtsas
5. besviken
6. krånglig
7. förmodligen
8. lagom
9. skärgård
10. Jag hörde ordet "rimlig" på jobbet

A/B artifacts:

- [backend/ab-results-20260707-230307.txt](backend/ab-results-20260707-230307.txt)
- [backend/ab-summary-20260707.md](backend/ab-summary-20260707.md)

Claude was chosen by user preference even though GPT recovered `skärgård` better in one test.

---

## What the next agent should do next

Start here:

1. **Respect the no-push instruction**
   - User said: `dont push anything until i say so`.
   - Do not commit or push unless explicitly told.
   - Current uncommitted files at this handoff:
     - [backend/web/index.html](backend/web/index.html)
     - [backend/web/styles.css](backend/web/styles.css)
     - [backend/web/app.js](backend/web/app.js)

2. **Implement requested keyboard shortcuts**
   - User asked whether they can use:
     - Up/down arrow keys to move between clips.
     - Spacebar to toggle selected clip between approved and in-review states.
   - This was requested but not implemented before this handoff update.
   - Recommended behavior:
     - Do not trigger shortcuts while focus is inside `input`, `textarea`, `select`, button, link, or audio controls.
     - Up/down should move `selectedClipId` through the current `clips` array and re-render.
     - Space should approve the selected clip if it is not approved.
     - Space should PATCH `{ "status": "needs_review" }` if the selected clip is already approved.
     - `ClipPatch` in [backend/server.py](backend/server.py) already includes optional `status`, so the PATCH path should work.
     - Consider updating the visible approve button label to reflect state, e.g. `Approve card` vs `Move to review`.

3. **Have the user visually check the web UI**
   - Start server with the command above.
   - Open `http://127.0.0.1:8080`.
   - Ask what feels wrong visually/functionally.

4. **Flash/test firmware touch**
   - CST816 touch is implemented and builds, but not yet flashed/tested on hardware.
   - Validate coordinates from logs before tightening hit targets.

5. **Implement device upload transport**
   - Add WiFi config strategy first: hardcoded dev config vs provisioning.
   - Target server URL likely Hoth LAN IP, e.g. `http://192.168.x.x:8080`.
   - Implement multipart upload to `/api/upload`.
   - Delete each WAV only after ACK.

6. **Polish server review workflow**
   - Add auto-process option after upload if user wants it.
   - Improve error display/loading states in web UI.
   - Add duplicate handling before Anki export.
   - Add direct AnkiConnect later.

7. **Deploy on Unraid**
   - Build and push `kliszaj/wordsnap:latest` from [docker-compose.build.yml](docker-compose.build.yml).
   - Use [docker-compose.unraid.yml](docker-compose.unraid.yml) on Hoth to pull the Docker Hub image.
   - Persist `/app/data` via `WORDSNAP_DATA_DIR`, likely `/mnt/user/appdata/wordsnap/data`.
   - Keep `backend/.env` private.

8. **Hardware caution**
   - Firmware builds but the latest touch behavior was not flashed/tested on hardware yet.
   - Be careful not to open/reset COM3 while the user is actively recording.

---

## Git / ignored files

Ignored local/runtime artifacts include:

- `backend/.env`
- `backend/.venv/`
- `backend/data/`
- `backend/*.log`
- `firmware/build/`
- `firmware/managed_components/`

Working tree is **not clean** at this handoff. The uncommitted changes are intentional local UI edits:

- `backend/web/index.html`
- `backend/web/styles.css`
- `backend/web/app.js`

Do not push them until the user says to.
