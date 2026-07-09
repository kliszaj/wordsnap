# WordSnap - Handoff

Repo: <https://github.com/kliszaj/wordsnap>

Goal: a small ESP32-C6 recorder captures Swedish words/phrases, syncs clips to Hoth/Unraid, transcribes with Whisper, enriches with Claude, and sends clean cards into Anki.

Current deployment target: Hoth at `10.0.0.240`.

---

## Status At A Glance

| Piece | State |
|---|---|
| Device recording | Working on hardware |
| SD organization | Recordings live under `/sdcard/clips/`; root recordings were auto-migrated |
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
| Docker | WordSnap compose/build files exist; latest image pushed to Docker Hub |

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
- Home UI has `SNAPS [n]` pill, red record button, and a bottom charging row when USB power is present.
- Charging row shows amber bolt + `CHARGING NN%` while charging, green bolt + `NN%` after charge completes; both use the live AXP2101 gauge.
- Touch red record button to record.
- Tap recording screen to stop.
- BOOT is power-only: press while asleep to wake, press while awake to sleep.
- BOOT does not start or stop recordings.
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
- Delete snips from the side panel.
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

Latest pushed image from this checkpoint:

```text
kliszaj/wordsnap:latest
sha256:719d7d27f7edcf62181a70ebc2208cc7d7efad446d76d9c63b3c1b8c9eb4e6c3
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

### Recent Work (session 2026-07-09)

**Backend / Web:**

- **Auto-sync toggle** — backend + web. `AUTO_ANKI` flag (default ON) in `backend/settings.py`
  (`auto_anki_enabled()`), gates the auto-save-to-Anki step in `auto_process_clip`, plus
  `GET`/`POST /api/auto-sync`. Header toggle switch in `index.html`/`app.js`/`styles.css`, left of
  Settings. Asset cache-buster is `?v=20260709-3`.

**Device firmware:**

- **Display tuning (firmware)** — ST7789 was washed out. Added the standard Waveshare
  VCOM/power/gamma register block after `esp_lcd_panel_init` in `lcd_init()` (deeper blacks,
  richer color). Knobs to tune further: `0xBB` VCOMS (→`0x28` for darker black), `0xC3` VRHS.
  *Built + flashed + user-approved.*
- **Screen transition fix (firmware)** — removed the ugly black interstitial on navigate/refresh.
  Was blanking the backlight during a slow line-by-line repaint. Now: `ui_transition_begin/end`
  are no-ops (repaint in place, backlight stays on) and `lcd_fill` paints 32-row strips
  (~284 SPI transactions → ~9). *Built + flashed + user-approved.*
- **Speaker removal** — the ES8311 playback path never drove the NS4150B amp correctly; hardware
  speaker is being removed. All playback/codec code is gated behind `#define SPEAKER_ENABLED 0`
  in `firmware/main/main.c` (globals, `audio_input_deinit`/`audio_output_init`/`audio_output_deinit`,
  `preview_clip_index`, `ui_show_playback`, `ui_playback_update_levels`, `draw_play_icon`). The
  per-clip **play button is removed** from the clips view (icon + touch region gated off; clip
  label shifted left). Flip the macro to `1` to restore.
- **Upload % animation (firmware)** — `ui_show_uploading` now tweens the big `N%` smoothly between
  per-clip progress steps set by `upload_progress`, repainting only the number area.
- **Event-driven idle + light sleep, button wake (firmware)** — PM is enabled in `sdkconfig` and
  `sdkconfig.defaults`; `app_main` configures 160/40 MHz with auto light sleep. When the screen is
  off, the main loop blocks in `esp_light_sleep_start()` and wakes from the BOOT button only
  (`GPIO9` low-level wake). When the screen is on, BOOT puts the screen back to sleep instead of
  acting as a recording shortcut. Touch wake while screen-off is intentionally disabled; touch remains
  responsive while the screen is on. Deep sleep is intentionally not used because C6 deep-sleep GPIO
  wake needs LP-GPIO 0–7, while the button is GPIO9 and touch INT is GPIO11.
- **Device copy + charging state (firmware)** — on-device wording is now `SNAPS` / `SNAP` / `NO SNAP`.
  Home charging UI is a bottom row, not a corner icon: amber bolt + live `CHARGING NN%` while charging,
  green bolt + live `NN%` when USB power is present but charging is complete. The green is `#58A75D`.
  Charging/done rows clear the full bottom strip before drawing to avoid overlap. *Built + flashed.*

**Planned (not started):**

- **Power verification** — measure idle current after screen-off, confirm instant button wake,
  confirm recording still has no I2S glitches, and confirm WiFi upload still succeeds with PM on.
- **Battery**: safe to connect a single-cell 3.7 V LiPo now — the AXP2101 PMIC charges it from USB
  automatically and the firmware already reads its gauge. **Check JST polarity against the board
  silkscreen first.** Real runtime needs the light-sleep work above.
- **Rename `wordclip` → `wordsnap` and internal `clips` → `snips`/`snaps`** across the codebase (firmware project
  name/binary, identifiers, API routes, SD `/sdcard/clips/` path + migration). Large,
  cross-cutting; the device↔server contract (`/api/clips`, upload paths) and the on-card folder must
  migrate together. Plan as its own pass.

Recommended next order:

1. Measure/verify the new firmware power behavior, charging row, and upload animation on hardware.
2. Pull/restart the newest WordSnap Docker image on Hoth if it has been pushed since this handoff.
3. Record a new device snap and run the full fresh path: record → upload → auto-process → save →
   phone sync.
4. Plan and execute the `wordclip→wordsnap` / `clips→snaps` rename as a dedicated pass.

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
