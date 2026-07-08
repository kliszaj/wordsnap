# WordSnap Product UI

## System Shape

WordSnap has two interfaces with one visual language.

- Device: capture and upload. It should be fast, low-text, and thumb-sized.
- Hoth website: review and correction. It should expose every clip/card state before anything reaches Anki.

The device is not the place to correct language. The website is.

## Device Screens

### Idle / Ready

- Charcoal rounded screen.
- Top pill: `Upload [12]`, where the number is pending local clips.
- Large red circular record button.
- Tap record: start a new recording.
- Tap upload: upload pending clips to the configured Hoth receiver.
- If the screen is asleep, the first tap wakes it instead of recording.

### Recording

- Dotted charcoal background.
- Huge elapsed timer, e.g. `00:12`.
- Lower-left `REC` with red dot.
- Lower-right clip index pill, e.g. `01`.
- Tap recording screen: stop and save.
- Hard cap still applies as a safety timeout.

### Saved

- Brief confirmation after stop.
- Then return to Idle / Ready with the pending count incremented.

### Connecting

- Blue/neutral connection state after tapping upload.
- Fails red if `/sdcard/wifi.txt` is missing or WiFi cannot connect.
- Clips remain on SD.

### Upload

- Progress bar for batch sync.
- Success state: green, clips deleted only after server ACK.
- Partial failure state: red, failed clips retained on device.
- Invalid or empty WAVs are skipped and removed locally.
- Server response time syncs the device RTC so future clips can carry useful timestamps.

### Error States

- No SD / SD mount failure: recording disabled.
- No WiFi config: show failure and keep clips.
- Server unreachable: show failure and keep clips.
- Partial upload: show failure and keep only unsynced clips.

## Hoth Website

### Inbox

The main page shows one row per clip/card.

Each row includes:

- upload filename
- status: `uploaded`, `processing`, `processed`, `needs_review`, `approved`, `exported`, `error`
- audio playback
- Whisper transcript
- Swedish item
- Anki front/back preview
- actions: transcribe, save to Anki, export CSV fallback

### Correction

The correction loop is review-first but upload processing is automatic:

1. Device sync uploads clips.
2. Hoth auto-transcribes/enriches clips in the background.
3. Listen to audio in the web UI.
4. Fix transcript or Swedish item if needed.
5. Edit final `SWE:` / `ENG:` if needed.
6. Save to Anki.

If AnkiConnect is configured, `Save to Anki` sends directly to the configured deck.
Otherwise it marks the card approved for CSV export.

## API Contract

Device-facing:

- `POST /api/upload`: multipart `file`, optional `capture_timestamp`, optional `device_id`, optional `auto_process`.
- Response includes `id`, `status`, and server time.
- `GET /api/device/status`: upload count and server time.

Website-facing:

- `GET /api/clips`
- `POST /api/clips/{id}/process`
- `PATCH /api/clips/{id}`
- `POST /api/clips/{id}/approve`
- `POST /api/clips/{id}/save-to-anki`
- `DELETE /api/clips/{id}`
- `GET /api/export/anki.csv`

## Visual Language

- Background: off-black/charcoal, never pure black.
- Accent: recording red.
- Secondary accent: muted blue-lavender text.
- Controls: rounded pills and large touch targets.
- Web UI should feel like the device grew up into a review dashboard, not like an unrelated admin panel.
