# PRD: Swedish Vocabulary Capture & Anki Pipeline

**Project codename:** Wordclip
**Author:** Adrian
**Status:** Draft — v2 (batch / offline-first, Unraid backend)
**Last updated:** 2026-06-18

---

## Problem

At B2 level, the most valuable Swedish vocabulary is no longer in textbooks — it's in the world. Words heard on the tunnelbana, in meetings, in shops, on TV. These words are encountered in context, which makes them more memorable, but only if they're captured and reviewed before the moment passes.

Current workarounds fail in the same way: they require too much friction at the moment of encounter. Opening Anki, Google Translate, or a notes app means unlocking a phone, context-switching, and risking a distraction spiral. The result is a graveyard of "words I meant to look up" that never get reviewed.

The gap is not in the review system — Anki works. The gap is in the capture-to-card pipeline.

---

## Goal

A dedicated handheld device that captures spoken Swedish words in the moment with a single long-press, stores them offline, and — once back home — offloads them to a self-hosted pipeline that transcribes, enriches, and turns each capture into an Anki card, sorted by the week it was captured.

The defining principle is **offline-first capture, batched processing**. Capture must work anywhere with zero connectivity and zero friction. All the intelligence (transcription, enrichment, card-building) happens later, in batch, on the home server.

---

## Users

**Primary:** Adrian — B2 Swedish learner, living in Stockholm, using Anki with an existing deck, self-hosted infrastructure on Unraid ("Hoth").

This is a personal tool built to a specific workflow. Generalisability is a non-goal.

---

## System overview

```
┌─────────────────────────┐
│  DEVICE (ESP32-C6)      │
│  • long-press to record │
│  • store WAV to Flash   │
│  • show storage left    │
│  • "sync" when home      │
└───────────┬─────────────┘
            │  WiFi batch upload (at home)
            ▼
┌─────────────────────────────────────────────┐
│  HOTH (Unraid)                              │
│                                             │
│  [1] Receiver service                       │
│       receives clips, queues them           │
│              │                              │
│              ▼                              │
│  [2] OpenAI Whisper API  ── Swedish text    │
│              │                              │
│              ▼                              │
│  [3] Enrichment (Claude/GPT) ── card JSON   │
│              │                              │
│              ▼                              │
│  [4] AnkiConnect ── card created,           │
│       tagged by ISO week                    │
│       (headless Anki container on Hoth)     │
└───────────────────────┬─────────────────────┘
                        │  AnkiWeb sync
                        ▼
              AnkiWeb  ⇄  phone / desktop (review-only)
```

---

## Component specifications

### Part A — The device (ESP32-C6-Touch-LCD-1.83)

**Hardware on hand:**
- ESP32-C6, single RISC-V core @ 160MHz
- 512KB HP SRAM, 16KB LP SRAM, 320KB ROM, **16MB external Flash**
- **No PSRAM** — this shapes the whole storage design
- Onboard audio codec + microphone
- 1.83" capacitive touch LCD, 240×284
- 6-axis IMU, RTC (PCF85063), LiPo charging
- WiFi 6 + BLE 5
- Programmable PWR and BOOT buttons

#### A1 — Capture

- **Trigger:** long-press (PWR or BOOT button, or on-screen touch). Long-press chosen over short-press to avoid accidental captures in a pocket.
- **Recording:** audio written **directly to Flash** via LittleFS as a WAV file. Cannot buffer in SRAM — 512KB is shared with display, WiFi stack, and program; even a few seconds of audio would exhaust it.
- **Format:** 16kHz, 16-bit, mono PCM WAV. ~32KB/sec.
- **Max clip length:** capped at **15 seconds** (~480KB). Fixed cap keeps storage math predictable and prevents runaway recordings.
- **Filename:** `YYYYMMDD-HHMMSS.wav`, timestamp from onboard RTC. The capture time travels with the clip — critical for weekly sorting later.
- **Feedback:** screen shows a recording indicator + elapsed seconds while held. Brief confirmation tone/flash on release.

#### A2 — Storage & display

- LittleFS partition carved from the 16MB Flash (see partition note below).
- **Storage display is a first-class screen.** Show:
  - Clips pending (e.g. `23 clips`)
  - Approx capacity used (bar + `~6 of 8 min`)
  - A clear **"nearly full"** warning state (e.g. <2 min remaining)
- Practical capacity: ~16 min of audio across the LittleFS partition → roughly 60–90 clips depending on length. Far more than a typical few-days-between-syncs cycle needs.

#### A3 — Offload / sync

- **User-initiated**, not automatic. A "Sync" button on the device UI.
- On sync: enable WiFi → connect to home network → upload each pending clip to the Hoth receiver → on per-clip success ACK, delete local copy → disable WiFi.
- **WiFi and audio capture never run simultaneously** — with no PSRAM and ~100–150KB free SRAM at runtime, the I2S audio path and WiFi stack can't coexist comfortably. Capture and sync are mutually exclusive modes.
- Sync screen shows progress (`uploading 4/23`) and a final summary (`23 synced, 0 failed`).
- Failed uploads are retained on device and retried next sync.

#### A4 — Firmware constraints to lock in early

- **Flash partition scheme** (`partitions.csv`): carve a dedicated LittleFS data partition alongside firmware. Decide sizes before writing other code.
- **Sequencing:** capture-mode and sync-mode as distinct states; never overlap WiFi + I2S.
- **RTC sync:** set device clock from server (NTP via the sync connection) so timestamps stay accurate. Drift matters for weekly bucketing.
- ESP-IDF ≥ v5.5.0 required for this board. LVGL version pinning is strict — match Waveshare's demo versions.

---

### Part B — Hoth pipeline (Unraid)

#### B1 — Receiver service

A small FastAPI app (Docker container on Unraid).

- **Endpoint:** `POST /upload` — accepts a WAV file + its filename (carrying the capture timestamp).
- Writes incoming clips to a queue folder, returns a per-clip success ACK so the device can delete its local copy.
- A worker then processes the queue: each clip runs through transcription → enrichment → card creation.
- Clips that fully process are moved to `processed/`; failures move to `errors/` with a log.

#### B2 — Transcription (OpenAI Whisper API)

- **Service:** OpenAI Whisper API (`whisper-1`), `POST /v1/audio/transcriptions`.
- **Decision:** cloud API over local Whisper. Adrian is willing to pay per-clip costs in exchange for zero server compute and no model management. Volume is low (a handful of words per day) so cost is negligible.
- **Parameters:**
  - `language=sv` — **transcription, not translation.** Whisper's translate mode only outputs English and would discard the Swedish being learned. We want Swedish audio → Swedish text.
  - `response_format=json`
  - Optional `prompt` parameter seeded with common Swedish orthography to nudge spelling of unfamiliar words.
- **Output:** raw Swedish transcript string, passed forward with the clip's capture timestamp.

> **Privacy note:** audio leaves the home network and goes to OpenAI. Acceptable for vocabulary clips; flagged here for completeness.

#### B3 — Enrichment

- **Method:** single call to an LLM (Claude `claude-sonnet-4-6` default; swappable for GPT-4-class).
- **Intent:** receive the raw Swedish transcript (possibly with a spoken context phrase), identify and correct the target word, and return structured linguistic metadata.

**Output schema (JSON):**

```json
{
  "raw_input": "tillmötesgående",
  "corrected_word": "tillmötesgående",
  "english_definition": "accommodating, obliging",
  "swedish_definition": "Villig att anpassa sig till andras önskemål eller behov.",
  "part_of_speech": "adjektiv",
  "gender": null,
  "forms": null,
  "example_sentence_sv": "Han var väldigt tillmötesgående när vi bad om hjälp.",
  "example_sentence_en": "He was very accommodating when we asked for help.",
  "register": "formal",
  "collocations": ["vara tillmötesgående", "tillmötesgående attityd"],
  "context_note": "Heard on the bus",
  "capture_timestamp": "2026-06-18T08:42:00",
  "iso_week": "2026-W25"
}
```

**Prompt requirements:**
- Infer and correct mispronounced or mis-transcribed words rather than substituting a different word
- Nouns: include gender (`en`/`ett`) and forms (obestämd/bestämd/plural)
- Verbs: include forms (infinitiv/presens/preteritum/supinum)
- Identify register (neutral/formal/informal/slang/literary)
- Extract a context note if a framing phrase was spoken
- Return only valid JSON, no preamble

#### B4 — Card generation (weekly sorting)

- **Method:** AnkiConnect `addNote` over HTTP — but pointed at a **headless Anki instance running on Hoth**, not the desktop Mac (see B4a).
- **Deck:** `Field Captures`
- **Weekly sorting:** each card is tagged with its **ISO week** derived from the capture timestamp (`2026-W25`), plus month (`2026-06`). This gives clean weekly buckets to filter and review — "what did I pick up this week."
- **Tags applied automatically:**
  - `field-capture`
  - `2026-W25` (ISO week — the primary sort key)
  - `2026-06` (month)
  - part of speech (`adjektiv`, `verb`, `substantiv`…)
  - register if non-neutral
- **Duplicate handling:** if the word already exists in the deck, skip and log.

#### B4a — Headless Anki on Hoth (always-on card sink)

The whole pipeline is always-on because Hoth is always on. The one piece that historically wasn't — AnkiConnect, which lives inside the desktop Anki app — is solved by running **Anki itself headless in a Docker container on Unraid**, so cards land the moment a clip is processed, with no desktop ever needing to be awake.

**Setup:**
- Anki desktop + the AnkiConnect add-on, running in a container under a virtual display (`Xvfb`). Community images exist for exactly this (e.g. headless-anki / anki-desktop-docker patterns).
- The container logs into Adrian's **AnkiWeb** account and syncs after every batch.
- AnkiConnect binds inside the container; the receiver service reaches it on the container's LAN address. No `webBindAddress` exposure to the Mac required — the card sink is now a server, not a desktop.
- Persist the Anki profile/collection on an Unraid volume so it survives container restarts.

**The critical operating rule — single writer:**
Anki's sync is collection-level and does **not** merge two sides that have both changed; a divergence forces a "which side wins" choice and can discard review history. To stay safe:
- **Hoth is the only client that *creates* cards.** It syncs to AnkiWeb immediately after each batch.
- Mobile (AnkiMobile/AnkiDroid) and any desktop Anki are **review-only** for this collection, and must **sync before and after** a review session so they never hold unsynced changes while Hoth writes.
- In practice the trickle is small and batches are quick, so collision windows are tiny — but the discipline matters and is non-negotiable for this setup.

**Why not the alternatives (recorded for posterity):**
- *Desktop AnkiConnect* — fails the always-on requirement; desktop must be awake.
- *genanki → `.apkg`* — always-on generation but **manual import**, just relocating the seam.
- *Direct AnkiWeb sync without the app* — fragile, unofficial, breaks on protocol changes.
- *Server-native SRS (non-Anki)* — architecturally cleanest, but leaves the Anki ecosystem. Rejected to stay in Anki.

**Card front:**
```
tillmötesgående

"Han var väldigt ___ när vi bad om hjälp."
```

**Card back:**
```
accommodating, obliging

Han var väldigt tillmötesgående när vi bad om hjälp.

adjektiv · formal
Villig att anpassa sig till andras önskemål eller behov.

Kolloktioner: vara tillmötesgående · tillmötesgående attityd

📍 Heard on the bus
```

#### B5 — Optional: TTS pronunciation audio

If enabled, generate a Swedish TTS clip (Azure/ElevenLabs) for the word + example sentence and attach via AnkiConnect `storeMediaFile`. Hearing correct pronunciation, not your own approximation, matters at B2. Disabled by default; config flag.

---

## Configuration

```toml
[device]
# baked into firmware / set via sync handshake
wifi_ssid = "..."
sample_rate = 16000
max_clip_seconds = 15

[receiver]
upload_port = 8080
queue_dir = "/data/queue"
processed_dir = "/data/processed"
errors_dir = "/data/errors"

[openai]
model = "whisper-1"
language = "sv"

[enrichment]
provider = "anthropic"        # anthropic | openai
model = "claude-sonnet-4-6"
max_tokens = 1000

[anki]
# headless Anki + AnkiConnect container on Hoth, not the desktop
host = "http://anki:8765"     # container hostname on the Unraid Docker network
deck = "Field Captures"
note_type = "Basic (and reversed card)"
ankiweb_sync = true           # container syncs to AnkiWeb after each batch

[features]
tts_audio = false
```

---

## Error handling

| Failure point | Behaviour |
|---|---|
| Clip upload fails mid-sync | Retain on device, retry next sync |
| Whisper API error / timeout | Requeue clip, retry with backoff; after N retries move to `errors/` |
| Whisper returns empty transcript | Log, move clip to `errors/`, do not enrich |
| Enrichment returns malformed JSON | Retry once; if still bad, save raw response to `errors/` |
| AnkiConnect (Hoth container) unreachable | Queue finished card JSON, retry on schedule; alert if container is down |
| AnkiWeb sync conflict | Halt writes, alert — indicates a non-Hoth client held unsynced changes (see single-writer rule) |
| Duplicate word | Skip card, log |
| Device Flash full | Block new captures, show "storage full — sync to clear" |

All server errors logged with timestamp and source filename.

---

## Build phases

### Phase 0 — Validate the pipeline with no hardware (this week)
Prove the card quality before soldering anything. Record a few Swedish words on a phone, drop the WAVs into the Hoth receiver folder manually, and run the full transcribe → enrich → card loop. **The enrichment prompt is what determines whether this is useful — iterate it here first.**

### Phase 1 — Hoth backend
- Receiver service (`/upload` + queue worker)
- OpenAI Whisper integration
- Enrichment call + JSON parsing
- **Headless Anki + AnkiConnect container** (Xvfb, AnkiWeb login, persistent volume)
- Card creation with ISO-week tagging via the container
- Single-writer sync discipline verified (Hoth writes, clients review-only)
- Error queues + retry logic

### Phase 2 — Device firmware
- LittleFS partition + WAV capture on long-press
- Storage display screen with "nearly full" warning
- Sync mode: WiFi batch upload + per-clip delete-on-ACK
- RTC time-set on sync
- Capture/sync mode separation

### Phase 3 — Polish
- TTS audio attachment
- On-device confirmation of capture count before sync
- Enclosure / clip

### Future — iOS app (alternative to Hoth as receiver)
The device firmware that uploads to a LAN endpoint is ~90% identical to firmware that uploads to a phone. Once the Hoth pipeline is proven and the card format is locked, an iOS app could replace the server as the receiver — making the whole pipeline portable (offload on the train, not just at home). Adrian has an iOS dev account and wants the practice. Tradeoffs to revisit then: on-device vs API Whisper, and the clunkier Anki-on-iOS sync story (AnkiMobile URL scheme / AnkiWeb rather than AnkiConnect). Deferred deliberately — porting a known-good pipeline is a focused project; inventing it in Swift alongside new firmware is two unknowns at once.

---

## Open questions

1. **Enrichment model:** Claude or GPT-4 for the enrichment step? Worth A/B-ing card quality on the same set of Swedish words, especially for accurate gender/forms and natural example sentences.

2. **Whisper accuracy on unknown-word pronunciation:** the hardest case is a word Adrian heard once and can't pronounce. Test how `whisper-1` handles non-native Swedish, and whether the enrichment LLM can reliably recover the intended word from a rough transcript. May warrant the optional `prompt` seeding.

3. **Note type:** built-in `Basic (and reversed card)`, or a custom `Swedish Vocabulary` note type with dedicated fields (gender, register, collocations)? Custom gives far better filtering and templating long-term for ~20 min of setup. Leaning custom.

4. **Context phrase convention:** encourage always speaking a Swedish framing phrase ("hörde på bussen — …"), or accept English context? Affects the enrichment prompt design.

5. **Sync trigger:** dedicated on-screen button vs automatic-on-known-WiFi. Manual is simpler and avoids the WiFi/audio coexistence problem, but auto is lower-friction. Manual for v1.

6. **RTC drift / timezone:** ensure capture timestamps land in the correct ISO week across the Europe/Stockholm timezone and DST. Set from server on each sync.

7. **Headless Anki container robustness:** the Xvfb/headless-Anki pattern works but can be brittle across Anki version bumps, and AnkiWeb login sessions occasionally need re-auth. Pin the Anki version in the container, persist the profile on a volume, and add a health check that alerts if the container falls out of sync. Confirm the single-writer rule holds in practice once mobile review is in the loop.

---

## Dependencies

**Device:** ESP-IDF ≥ v5.5.0, LittleFS, LVGL (version-pinned to Waveshare demos), ESP32 audio codec driver.

**Hoth:**

| Package | Purpose |
|---|---|
| `fastapi` + `uvicorn` | Receiver service |
| `openai` | Whisper API client |
| `anthropic` | Enrichment (default provider) |
| `requests` | AnkiConnect calls |
| `python-dateutil` | ISO week derivation |
| `ffmpeg` | Audio normalisation if needed |

---

*v2 supersedes the software-only v1. Device capture is now central; transcription moved from local faster-whisper to the OpenAI Whisper API; cards are sorted by ISO week; card creation now targets a headless, always-on Anki container on Hoth rather than the desktop. iOS app retained as a future alternative backend.*
