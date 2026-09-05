# WordSnap - Handoff

Repo: <https://github.com/kliszaj/wordsnap>

Goal: a small ESP32-C6 recorder captures Swedish words/phrases, syncs clips to Hoth/Unraid, transcribes with Whisper, enriches with the preferred LLM provider, and sends clean cards into Anki.

Current deployment target: Hoth at `10.0.0.240`.

## Firmware Safety, Performance, and Polish Pass (2026-07-18)

Implemented and verified as part of the current repository checkpoint. The complete pass was built and flashed to COM3 with `firmware\flash.ps1`.

- SD mount failures can no longer auto-format the card.
- Recording now checks free space and every write, checkpoints a valid WAV header while capturing, flushes durably, and atomically renames `rec_NNN.part` to `.wav` only after success.
- Boot repairs interrupted `.part` recordings from their actual file size. Non-empty files that cannot be repaired are retained.
- Clip indexing uses a 125-byte bitmap instead of 4-5 KB stack arrays and reports `STORAGE FULL` instead of overwriting `rec_999.wav`.
- PMIC rail/charger initialization now runs before LCD, SD, audio, and touch. Audio gets bounded clean retries; unavailable SD/audio/touch peripherals retry every five seconds while idle.
- The onboard PCF85063 RTC restores system time at boot and is updated whenever Hoth returns server time.
- Ordinary PWR wake uses fast panel-on. A USB/battery handoff marks the ST7789 for the proven full reset/tuning path before backlight-on.
- Solid rectangles render in 16-line tiles, font rows render contiguous runs, and line primitives avoid per-point discs. This removes a large number of tiny synchronous SPI transfers without allocating a full framebuffer.
- Firmware now tracks explicit Home, Recording, Saving, Snap List, Connecting, Uploading, Error, and Sleep states. Invisible Home hit zones no longer remain active over error screens.
- Recording shows immediate `SAVING` feedback before `DONE`; the waveform is the only animated recording progress visual, while the 15-second automatic stop remains enforced without a separate red rail.
- Snap rows show only their centered three-digit index. Delete uses a two-second soft-delete with `UNDO`; leaving the screen or expiry commits it.
- Label pills use measured glyph bounds for centering. The 44 px Home/Upload pills give tall bracket glyphs more room, and the recording index plus Back/Undo labels are centered within their actual containers.
- WiFi connection animates its arcs. Upload writes handle partial transport sends, invalid WAVs are retained, and completion reports sent/retry counts rather than reducing every partial failure to `NO SERVER`.
- Charging-complete green appears only for the PMIC's actual done state, not every plugged-in noncharging condition.
- Speaker playback was permanently removed after the hardware speaker was dropped: the ES8311/I2S output path, playback UI and touch actions, output pin definition, and stale speaker documentation are gone. The codec build now enables only the ES7210 microphone input driver.
- Battery standby now has two levels: the screen enters quick light-sleep standby after 30 seconds, then battery-only idle requests true AXP2101 shutdown after 10 minutes. PWR polling slows from 250 ms to one second after the first minute; USB-powered operation never auto-shuts down. A short PWR press wakes quick standby or cold-boots after PMIC shutdown.

Verification completed:

- `idf.py build` passes with no compiler warnings.
- The speaker-free firmware rebuilt and flashed successfully to COM3; the build compiled only the ES7210 codec driver.
- Firmware image: `0x133e20` bytes, 18% of the smallest app partition remains.
- DIRAM: 204,522 / 452,112 bytes (45.24%), leaving 247,590 bytes before runtime allocations.

Next physical test:

1. Record/stop twice and confirm `REC -> SAVING -> DONE -> Home` has no blank or clipped frame.
2. Interrupt one recording with reset, reboot, and confirm the recovered snap appears in the numbered list.
3. Delete a snap, test `UNDO`, then delete again and wait two seconds.
4. Upload a mixture of valid and empty snaps and confirm valid snaps leave the card while invalid/failed ones remain with a retry result.
5. Sleep/wake normally, then repeat across USB plug/unplug to compare fast wake versus full recovery wake.
6. Power-cycle before WiFi sync and verify new file timestamps come from the PCF85063.
7. Leave the unplugged device asleep for more than 10 minutes, confirm it cold-boots from one short PWR press, then run a multi-day battery-life test.

## Active Firmware Robustness Pass (2026-07-13)

This pass is incorporated in the current firmware robustness checkpoint.

Completed and build-verified so far:

- After a reported glitchy wake following a true 0%-to-full charge cycle, the PMIC setup was compared against Waveshare's official BSP. Startup now explicitly restores DC1, ALDO1, and ALDO2 to 3.3 V; a fully depleted cell can no longer leave those rails dependent on retained AXP2101 register state.
- Every screen wake now hardware-resets and fully reinitializes the ST7789, reapplies its color/power tuning, paints the next frame with the backlight dark, and only then enables the backlight. This recovers cleanly if USB removal disturbed the LCD controller while the ESP remained alive.
- LCD color transfers now have completion tracking. Drawing buffers are not reused until queued SPI DMA work finishes, removing a source of stray colors/lines.
- Cold boot now keeps the backlight off through LCD initialization and turns it on only after the complete `STARTING` frame has been painted, so uninitialized panel memory is never exposed.
- Screen sleep turns both the backlight and ST7789 display output off.
- Wake keeps the backlight dark until the panel is enabled and the next view is fully painted.
- The low-battery fill clamps its corner radius to the actual fill dimensions, so narrow red/amber fills stay inside the battery outline.
- Startup failures now show `NO SD` or `NO AUDIO`; solid magenta is no longer used as an unexplained error state.
- AXP2101 startup now enables battery detection/voltage ADC, disables unused TS sensing, and configures the 400 mAh LiPo conservatively at 200 mA charge current, 25 mA termination, and 4.20 V target.
- Battery state is now one validated read containing presence, VBUS, charging phase, voltage, and percentage. Gauge values are cross-checked against cell voltage and filtered across plug/unplug transitions; failed reads no longer become a fake `50%`.
- Charging UI appears only when a battery is actually detected, and both charging and completed percentages refresh when they change.
- ES7210/I2S is now an on-demand resource: it is initialized and self-tested at boot, suspended during idle, resumed just before recording, and suspended again after the WAV closes or recording setup fails.
- The post-record `DONE` view no longer redraws Home itself; the main event loop performs the single Home render, removing the brief black frame caused by two consecutive full-screen paints.
- The remaining post-record blank frame was traced to Home's full-screen clear. Successful recordings now transition section-by-section from `DONE` into the Home controls, keeping visible content on screen while each region is replaced.
- VBUS now holds a dedicated no-light-sleep PM lock. This keeps the native USB Serial/JTAG interface stable while the cable is attached without affecting battery idle behavior when unplugged.
- `firmware/flash.ps1` provides the normal Windows flash path using ESP32-C6 `usb_reset`; no BOOT/PWR sequence is required once this firmware is installed.
- Blank-screen idle no longer wakes every 20 ms or emits one-second heartbeat logs. It sleeps in 250 ms windows between AXP2101 PWR polls; touch is not polled while blank.
- Long PWR shows `POWERING OFF`, suspends audio, and sends the AXP2101 hardware shutdown command (`COMMON_CONFIG` bit 0). If VBUS policy leaves the CPU running, it falls back to a recoverable panel/backlight-off state.
- CST816 remains electrically powered because the board leaves its reset pin unmanaged and the installed driver has no supported sleep/resume implementation. It is skipped entirely while the screen is off.

Connected-device status:

- The explicit PMIC-rail and wake-time LCD reinitialization build compiled and flashed successfully through `firmware/flash.ps1`; physical USB-to-battery wake verification is pending.
- The cold-start backlight fix is physically verified: the user confirmed the startup artifact is gone.
- The section-by-section post-record transition was built and flashed successfully. Its regional repaint order now clears every overlapping area before drawing the Home record control last, preventing the lower half from being erased; physical verification is pending.
- A second complete flash using `firmware/flash.ps1` succeeded with automatic USB reset at 460800 baud, proving future development flashes no longer require button choreography.
- The first complete robustness build flashed successfully over normal `idf.py -p COM3 flash` and booted without LCD/PMIC/audio initialization faults.
- Live telemetry showed AXP gauge `38%` and charging cell voltage `3934 mV`. This exposed an important policy correction: while VBUS is present the valid PMIC gauge remains authoritative because charge voltage is elevated; voltage may replace a wildly inconsistent gauge only while unplugged.
- The final gauge-policy correction was flashed successfully through the manual bootloader path; all flash regions passed hash verification. Avoid reopening serial during visual testing because native USB serial can reset the C6 or leave `COM3` temporarily non-configurable.

Physical verification checklist after the final flash:

1. Home paints cleanly with no pink field, stray lines, or partial-frame artifacts.
2. Wait 30 seconds: panel and backlight turn off. Short PWR wakes a fully painted Home frame; screen taps do not wake it.
3. Record a snap: recording starts promptly, waveform moves, WAV saves, and Home returns without a black flash. Repeat once to prove audio resume/suspend is reusable.
4. Unplug USB and wake: no battery overlay above 30%; amber at 15-30%; red below 15%; fill remains within its outline.
5. Plug/unplug several times: percentage changes smoothly rather than jumping from full to empty.
6. Long PWR: `POWERING OFF` appears, then PMIC removes processor/display power. Short PWR starts it again.
7. Upload one snap and confirm screen stays awake and WiFi upload still completes.
8. Measure current in Home, screen-off idle, recording, WiFi upload, and PMIC-off states before estimating final 400 mAh runtime.

---

## Status At A Glance

| Piece | State |
|---|---|
| Device recording | Working on hardware |
| SD organization | Recordings live under `/sdcard/clips/`; root recordings were auto-migrated |
| Batch sync | Working; tap upload pill to upload all clips, progress bar turns green on clean sync |
| Delete-on-ACK | Working; device deletes local WAV only after server 2xx ACK |
| Empty/invalid WAV handling | Backend rejects them; firmware retains them for explicit review/delete |
| Burn-in protection | Idle screen sleeps after 30s; only short PWR wakes it |
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

- Product name: `WordSnap` / web page title and header: `wordsnap`
- Enrichment provider: configurable in Settings via `LLM_PROVIDER`.
- Allowed enrichment providers are `anthropic` and `openai`; the non-preferred provider is used as fallback.
- Claude model default: `claude-sonnet-5`.
- Transcription: OpenAI Whisper API, default `whisper-1`.
- GPT enrichment default: `gpt-5.5`.
- Important: transcription still requires OpenAI/Whisper when a transcript is not already supplied, even if Anthropic is the preferred enrichment provider.
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
- BOOT/debug button: GPIO9; user power control is the AXP2101 PWR key
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
- Short PWR wakes/sleeps the screen; hold PWR for PMIC shutdown.
- BOOT does not control recording or normal sleep/wake.
- Max recording duration is capped.
- Upload pill batch-syncs all valid clips.
- Upload percentage updates during sync and can jump directly to the latest value/100% when upload finishes quickly.
- Upload keeps the screen awake and holds an `ESP_PM_NO_LIGHT_SLEEP` lock until sync finishes or errors.
- Connecting state shows a centered WiFi icon with amber `CONNECTING...`.
- Invalid/empty WAVs are retained locally for explicit review/delete.
- Firmware includes capture timestamp when file mtime is valid.
- Upload response syncs system time and persists it to the onboard PCF85063 RTC.

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

- Page title/header is `wordsnap` in red; no `Hoth receiver` eyebrow.
- Header only keeps Auto-sync and Settings; the old metric strip, Upload button, and Export CSV button are removed.
- Settings lets the user choose Anthropic-first or OpenAI-first enrichment; the other provider is used as fallback.
- Left snip list scrolls independently.
- Delete snips from the side panel.
- Audio playback for selected snip.
- Two-card pipeline:
  - `01 transcription` lets the user correct the transcript and Swedish item.
  - Center `Sync` button processes the edited transcript/card front through Claude, then saves/syncs to Anki.
  - `02 anki` previews the generated card front/back.
- Translation column, Transcribe button, and Save-to-Anki button were removed from the visible pipeline.
- Keyboard shortcuts:
  - ArrowUp / ArrowDown navigate clips.
  - Space toggles save/review internally, but this should be revisited now that the UI is Save-to-Anki first.
- Polls while uploaded/processing clips exist.
- Cache-busted assets in `index.html` to avoid stale Hoth browser JS:
  - `styles.css?v=20260709-7`
  - `app.js?v=20260709-7`

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
LLM_PROVIDER=anthropic
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
sha256:658c72c3efc1c11fb03b2bb9851a9a01aa4c657a17fc496e6fd30f8502d45f78
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
  Settings. Asset cache-buster is `?v=20260709-7`.
- **Web header/pipeline simplification** — header now says red `wordsnap`; removed `Hoth receiver`,
  dashboard metrics, Upload, and Export CSV. Pipeline is two columns (`01 transcription`, `02 anki`)
  with a centered `Sync` button between them. `Sync` processes the corrected transcript/front and
  then saves/syncs to Anki. The visible Translation column, Transcribe button, and Save-to-Anki button
  are gone.
- **LLM provider preference + fallback** — Settings now includes `LLM_PROVIDER` (`anthropic` or
  `openai`). `process_clip` enriches with the preferred provider first and automatically falls back to
  the other provider if the preferred model/key/API call fails. The successful provider is stored on
  the clip as `llm_provider`; any first-provider error is stored as `llm_fallback_error`.

**Device firmware:**

- **Display tuning (firmware)** — ST7789 was washed out. Added the standard Waveshare
  VCOM/power/gamma register block after `esp_lcd_panel_init` in `lcd_init()` (deeper blacks,
  richer color). Knobs to tune further: `0xBB` VCOMS (→`0x28` for darker black), `0xC3` VRHS.
  *Built + flashed + user-approved.*
- **Screen transition fix (firmware)** — removed the ugly black interstitial on navigate/refresh.
  Was blanking the backlight during a slow line-by-line repaint. Now: `ui_transition_begin/end`
  are no-ops (repaint in place, backlight stays on) and `lcd_fill` paints 32-row strips
  (~284 SPI transactions → ~9). *Built + flashed + user-approved.*
- **Upload % animation (firmware)** — `ui_show_uploading` now updates the big `N%` without forcing
  every intermediate percent. Small changes get a short glide, large jumps and 100% draw immediately,
  repainting only the number area.
- **Upload keep-awake (firmware)** — sync now sets `s_upload_active`, refreshes the activity timer on
  progress updates, blocks idle screen sleep while uploading, and holds an `ESP_PM_NO_LIGHT_SLEEP`
  lock so PM auto light-sleep does not interrupt WiFi upload. *Built + flashed.*
- **Connecting icon (firmware)** — the connecting view now draws a centered WiFi icon instead of the
  old `...` dots. *Built + flashed.*
- **Low-power idle + PWR wake (firmware)** — PM is enabled in `sdkconfig` and
  `sdkconfig.defaults`; `app_main` configures 160/40 MHz with automatic light sleep. The board does not
  expose the AXP2101 IRQ pin, so blank-screen idle checks latched PMIC PWR events every 250 ms while
  automatic light sleep handles the gaps. Touch wake is intentionally disabled to prevent pocket wakes.
- **Device copy + charging state (firmware)** — on-device wording is now `SNAPS` / `SNAP` / `NO SNAP`.
  Home charging UI is a bottom row, not a corner icon: amber bolt + live `CHARGING NN%` while charging,
  green bolt + live `NN%` when USB power is present but charging is complete. The green is `#58A75D`.
  Charging/done rows clear the full bottom strip before drawing to avoid overlap. *Built + flashed.*
- **Wake battery overlay thresholds (firmware)** — when unplugged, wake shows the battery overlay only
  at or below 30%. Red is below 15%; amber/yellow is 15-30%; above 30% shows no battery overlay.

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
