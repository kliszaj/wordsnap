# Wordclip — Handoff

Context for picking up this project. Goal (per [prd-wordclip.md](prd-wordclip.md)): a handheld
ESP32-C6 device that captures spoken Swedish words → offloads to a self-hosted pipeline that
transcribes (Whisper) → enriches (LLM) → builds Anki cards sorted by ISO week.

**This session's mission:** get audio recording working on the Waveshare board to judge audio
quality, then start the Phase 0 backend to decide **Claude vs GPT** for enrichment.

---

## Status at a glance

| Piece | State |
|---|---|
| Device firmware — record to SD | ✅ **Working & verified on hardware** |
| Audio quality validated | ✅ Intelligible; user happy after gain bump + click fix |
| Phase 0 backend (Whisper → Claude vs GPT A/B) | 🚧 **Partially scaffolded** — see "What's left" |
| Hoth receiver / AnkiConnect / full pipeline | ⬜ Not started (later phases) |

**Recorded clips** `rec_001.wav`, `rec_002.wav`, `rec_003.wav` exist on the user's microSD card
(16 kHz/16-bit stereo, ES7210 dual-mic). `rec_003.wav` is the best (37.5 dB gain, click-free).
These are the real inputs for the backend A/B.

---

## Hardware & environment (confirmed facts)

Board: **Waveshare ESP32-C6-Touch-LCD-1.83**. On **COM3** (native USB-Serial-JTAG). LCD 240×284 ST7789.

- **ESP-IDF v5.5** installed at `C:\Users\Adrian\esp\esp-idf`. Activate + build:
  `cmd /c "call C:\Users\Adrian\esp\esp-idf\export.bat && idf.py -C <proj> ..."`
- Firmware project: [firmware/](firmware/) (target esp32c6, already `set-target`).
- **Confirmed pins** (schematic Rev1.2 + Waveshare examples): shared SPI2 bus SCLK=1 MOSI=2 MISO=16;
  LCD CS=5 DC=3 RST=4 BL=6; SD CS=17; I2S MCLK=19 BCLK=20 WS=22 DIN=21; codec I2C SDA=7 SCL=8;
  BOOT button GPIO9; ES7210 mic-array ADC @ I2C 0x40; ES8311 (playback) also on the bus.

### Dev-loop gotchas (these cost time — see [memory](../../.claude/projects/c--Users-Adrian-Documents-Coding-wordclip/memory/wordclip-firmware-devloop.md))
- **USB-Serial-JTAG reset:** pyserial RTS/DTR toggling does NOT reset the C6. Reset with esptool:
  `python -m esptool --chip esp32c6 -p COM3 --before default_reset --after hard_reset flash_id`.
  `idf.py monitor` fails (no TTY in this shell) — read serial with the pyserial scripts in the
  scratchpad (`cap.py`, `reset_read.py`). Firmware prints a **1 Hz heartbeat** so status is
  observable anytime; a reset re-enumerates USB so read AFTER it settles (~2.5 s).
- **SD must be FAT32 + MBR.** 64 GB cards ship exFAT → silent mount fail (`sd=FAIL`, magenta screen).
  Reformatted with Rufus → "Large FAT32", MBR. Reseat firmly (push-push slot) — a loose card also fails.
- **AXP2101 PMIC init NOT needed** — audio/SD/LCD rails are on by default at boot (all Waveshare
  examples run without touching it). Kept as a contingency only; never needed.

---

## Firmware (Stage A + B, both verified)

Single file [firmware/main/main.c](firmware/main/main.c). Flow: boot → LCD status → mount SD →
init ES7210 → **press BOOT = record ~10 s from the dual-mic array → WAV to `/sdcard/rec_NNN.wav`**.
LCD states: blue=boot, **green=ready**, magenta=SD/audio fail, **red+progress bar=recording**,
white flash=saved. Audio: 16 kHz / 16-bit / 2-ch (MIC1+MIC2) via TDM, `esp_codec_dev` (es7210),
mic gain **37.5 dB**.

- ES7210 pattern adapted from ESP-IDF example `examples/peripherals/i2s/i2s_codec/i2s_es7210_tdm`.
- [firmware/main/format_wav.h](firmware/main/format_wav.h) copied from `i2s_examples_common` (CC0).
- Deps: [firmware/main/idf_component.yml](firmware/main/idf_component.yml) pins `espressif/esp_codec_dev`.
- **Click fix (done):** clicking was I2S DMA starvation from full-screen LCD redraws during record.
  Fixed by (a) drawing only the incremental progress-bar slice, (b) bigger I2S DMA buffer
  (`dma_desc_num=8`, `dma_frame_num=1023`), (c) draining stale DMA before each capture. Verified gone.

Build & flash: `idf.py -C <firmware> -p COM3 flash` (NOT `flash monitor` — TTY error), then reset
via esptool and read heartbeat to confirm `sd=1 audio=1`.

**Possible future firmware work (not required now):** LittleFS/internal-Flash storage, WiFi sync,
RTC-timestamped filenames + ISO-week, LVGL text UI / storage-status screen, software AFE/noise
reduction (ES7210 supports it; we capture raw), on-device playback.

---

## Backend — Phase 0 A/B (IN PROGRESS)

Location: [backend/](backend/). Design: one WAV → Whisper (`language=sv`) → feed the **same
transcript + same prompt** to **both** Claude and GPT → print cards side by side to judge quality.

**Decisions:** Claude side = `claude-sonnet-5` (user's pick; PRD's `claude-sonnet-4-6` is
superseded). GPT side = a current GPT-4-class model, default `gpt-4o` (config var — confirm/adjust
to the user's OpenAI access, e.g. gpt-4.1). Whisper = `whisper-1`. User HAS both OpenAI + Anthropic
keys; read from env `OPENAI_API_KEY` / `ANTHROPIC_API_KEY` (do not paste secrets in chat).
Claude code uses the **anthropic SDK** (`messages.parse` + Pydantic); GPT/Whisper use the **openai
SDK** — kept in separate modules on purpose (per the claude-api skill).

**Files written:**
- [backend/schema.py](backend/schema.py) — Pydantic `Card` (PRD B3 fields; capture_timestamp/iso_week
  are derived in Python, not by the LLM).
- [backend/prompt.py](backend/prompt.py) — shared `SYSTEM` prompt + `user_prompt()`.
- [backend/enrich_claude.py](backend/enrich_claude.py) — `enrich_with_claude(transcript, model)`.
- [backend/enrich_openai.py](backend/enrich_openai.py) — `transcribe(wav, model)` + `enrich_with_gpt(...)`.

**What's left (next agent, start here):**
1. Write `backend/compare.py` — CLI `python compare.py <wav>`: `transcribe()` → print transcript →
   `enrich_with_claude` + `enrich_with_gpt` → render both cards. Derive capture_timestamp/iso_week
   from the file mtime (`datetime.fromtimestamp(os.path.getmtime(wav)).isocalendar()` → `YYYY-Www`)
   since our filenames aren't timestamped yet. Model IDs from env with the defaults above.
   Best-effort `from dotenv import load_dotenv; load_dotenv()`.
2. Write `backend/requirements.txt` (`anthropic`, `openai`, `python-dateutil`, `python-dotenv`,
   `pydantic`) and `backend/.env.example` (the two API keys + optional `GPT_MODEL`).
3. Create a venv with **Python 3.12** (`C:\Users\Adrian\AppData\Local\Programs\Python\Python312\python.exe`
   — avoid 3.14, wheels may lag) and `pip install -r requirements.txt`.
4. Run `python compare.py <path-to-rec_003.wav>` once the user sets env keys and gives the WAV path
   (the card is currently in their PC after listening; ask for the path or have them drop it in backend/).
5. Verify `messages.parse` works with Sonnet 5's default adaptive thinking; if it errors on token
   budget, either bump `max_tokens` or add `thinking={"type":"disabled"}`. Then let the user judge
   Claude vs GPT card quality → that closes the original question.

---

## Open decisions still to make (from PRD)
Enrichment model (this A/B answers it), custom vs built-in Anki note type (leaning custom),
context-phrase convention, TTS audio (off by default). Not blocking Phase 0.
