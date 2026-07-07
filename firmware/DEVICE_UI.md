# Device UI Implementation Notes

The firmware now has the WordSnap UI state machine, but touch and WiFi upload still need the board-specific driver pass.

## Current Behavior

- BOOT button mirrors the future touchscreen record button.
- Home view draws:
  - upload pill touch target
  - large red record circle
- Recording view draws:
  - dark dotted background
  - red REC indicator dot
  - clip-index pill placeholder
- Press BOOT to start recording.
- Release, then press BOOT again to stop before the safety cap.
- WAV header is rewritten after stop so short recordings stay valid.

## Next Firmware Pass

1. Confirm the touch controller model and pins from the Waveshare example/schematic.
2. Implement `touch_record_pressed()` and `touch_upload_pressed()` in `main.c`.
3. Add text rendering or LVGL so the pill can show `Upload [12]` and recording can show `00:12`.
4. Wire WiFi credentials and upload transport:
   - `POST /api/upload`
   - multipart field `file`
   - optional `capture_timestamp`
   - optional `device_id`
5. Delete each clip only after the server ACK includes an `id` and `status`.

The upload code should remain outside recording mode. WiFi and I2S capture should not run at the same time on this board.
