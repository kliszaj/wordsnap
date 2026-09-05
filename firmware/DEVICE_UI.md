# Device UI Implementation Notes

The firmware UI now follows the supplied 240x284 WordSnap device mocks.

## Visual System

- Background: near-black full-screen field.
- Typography: generated bitmap glyphs from local PP Supply Sans:
  - `PPSupplySans-Regular.otf` for labels and pills.
  - `PPSupplySans-Ultralight.otf` for timer and upload percentage.
- Generated font asset: `firmware/main/ui_font.h`.
- Primary colors:
  - red recording/accent
  - green done/upload status
  - amber connecting/medium battery
  - lavender-gray text

## Current States

- **Idle / main**
  - `Snaps [n]` pill centered near top.
  - Shows `Snaps [0]` when there are no pending snaps.
  - Large red record button with dark ring.
  - Bottom charging row appears only while USB power is present.
  - Amber bolt + `CHARGING NN%` uses the live AXP2101 battery gauge while active charging.
  - Green bolt + `NN%` uses the same live gauge when USB is present but charging has finished.
- **Snap menu**
  - Top `Upload [n]` pill uploads all pending snaps.
  - Shows two full snap rows plus a partial third row so tap targets stay large.
  - Each row shows only its three-digit local snap number (`001`, `002`, ...).
  - A right-side red X soft-deletes the snap and exposes `Undo` for two seconds.
  - Bottom row has up arrow, `Back`, and down arrow navigation.
- **Recording**
  - Large `00:SS` timer.
  - Level-bar motif on the lower left.
  - Bottom rail with red `REC` status and snap number pill.
  - Timer redraws once per second.
  - The waveform is the only animated recording progress visual; recording still stops automatically at 15 seconds.
- **Saving**
  - Replaces `REC` immediately after stop while the WAV is finalized durably.
- **Done recording**
  - Keeps the elapsed timer on screen briefly.
  - Bottom rail shows green `DONE` and snap number.
- **Wake battery overlay**
  - On wake from idle-off, shows battery for 2 seconds only when unplugged and at or below 30%.
  - Red appears below 15%; amber appears from 15% through 30%.
  - Above 30%, no battery overlay is shown.
  - Reads AXP2101 fuel gauge register `0xA4`; failed reads show no invented percentage.
- **Connecting**
  - Center WiFi icon builds its arcs while association is in progress.
- **Uploading**
  - Large percentage plus green `UPLOADING...`.
- **Done uploading**
  - Large `100%` plus green `DONE`.
- **No WiFi**
  - ASCII-inspired face rendered as `ಠ▃ಠ` motif.
  - Red `NO WIFI`.
- **No Server**
  - Same face.
  - Red `NO SERVER`.

## Touch Behavior

- Tap the red record target to start recording.
- Tap the recording screen to stop after the guard interval.
- PWR is power-only: short press while awake enters quick standby; short press during the first 10 minutes wakes immediately.
- After 10 minutes asleep on battery, the AXP2101 powers the board fully off. The same short PWR press then performs a normal cold boot.
- Automatic full shutdown is disabled while USB power is connected.
- Hold PWR for PMIC shutdown. BOOT remains for flashing/debug only.
- Tap `Snaps [n]` to manage pending snaps.
- Tap `Upload [n]` inside the snap menu to batch sync only when pending snap count is greater than 0.
- Tap snap-menu up/down arrows to page through local snaps.
- Buttons give immediate pressed feedback:
  - the home record button pulses before entering recording,
  - pills briefly invert,
  - snap-menu play/delete/nav controls flash a small ring.

## Upload Behavior

- Snaps are stored in `/sdcard/clips/`.
- Upload reads WiFi/server config from `/sdcard/wifi.txt`.
- Default server is `http://10.0.0.240:8090`.
- Upload is multipart `POST /api/upload`.
- Local snap is deleted only after server 2xx ACK.
- Recordings are checkpointed as `.part` files and atomically renamed after a durable close.
- Interrupted recordings are repaired from their actual size on the next boot.
- Invalid/empty WAVs are retained for review instead of silently deleted.
- WiFi/server failures retain every unacknowledged snap and report sent/retry counts.
- Server time is persisted to the onboard PCF85063 RTC and restored at boot.

## Next Visual QA

1. Flash to the device.
2. Photograph each state against the mock PNGs:
   - main
   - recording
   - done
   - connecting
   - uploading
   - done uploading
   - no wifi
   - no server
   - low/medium battery
3. Tune x/y positions and font sizes from real-screen photos.
