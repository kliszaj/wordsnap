/*
 * Wordclip capture-test firmware  --  Stage B
 * Board: Waveshare ESP32-C6-Touch-LCD-1.83
 *
 *   - LCD status display styled after the WordSnap UI mockups
 *   - Home view: upload pill + large red record button
 *   - Recording view: dark dotted field + stop-capable capture
 *   - microSD (FAT32) over shared SPI2 bus
 *   - Touch controls the active UI; the physical PWR button wakes/blanks the LCD.
 *     BOOT remains available only for startup/debug.
 *
 * Audio: ES7210 4-ch ADC (we use MIC1+MIC2), I2S TDM 2-slot, 16 kHz / 16-bit stereo.
 * Pins (schematic Rev1.2 + Waveshare examples):
 *   SPI2 shared: SCLK=1 MOSI=2 MISO=16 | LCD CS=5 DC=3 RST=4 BL=6 | SD CS=17
 *   I2S:  MCLK=19 BCLK=20 WS=22 DIN=21 | Codec I2C: SDA=7 SCL=8 | ES7210 @ 0x40
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/i2s_tdm.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "freertos/event_groups.h"
#include "format_wav.h"
#include "ui_font.h"

static const char *TAG = "wordclip";

/* ---- SPI / LCD / SD pins ---- */
#define PIN_SCLK    1
#define PIN_MOSI    2
#define PIN_MISO    16
#define PIN_LCD_CS  5
#define PIN_LCD_DC  3
#define PIN_LCD_RST 4
#define PIN_LCD_BL  6
#define PIN_SD_CS   17
#define PIN_TOUCH_INT 11
#define PIN_TOUCH_RST -1       /* Waveshare example leaves CST816 reset unmanaged; GPIO4 is LCD reset */
#define AXP2101_ADDR 0x34
#define AXP2101_STATUS1 0x00
#define AXP2101_STATUS2 0x01
#define AXP2101_COMMON_CONFIG 0x10
#define AXP2101_ADC_CTRL 0x30
#define AXP2101_BAT_V_H 0x34
#define AXP2101_BAT_V_L 0x35
#define AXP2101_INTEN2 0x41
#define AXP2101_INTSTS2 0x49
#define AXP2101_TS_PIN_CTRL 0x50
#define AXP2101_PRECHARGE 0x61
#define AXP2101_CHARGE_CURRENT 0x62
#define AXP2101_TERMINATION 0x63
#define AXP2101_TARGET_VOLTAGE 0x64
#define AXP2101_BAT_DETECT 0x68
#define AXP2101_DC_ONOFF 0x80
#define AXP2101_DC1_VOLTAGE 0x82
#define AXP2101_LDO_ONOFF0 0x90
#define AXP2101_ALDO1_VOLTAGE 0x92
#define AXP2101_ALDO2_VOLTAGE 0x93
#define AXP2101_BAT_PERCENT 0xA4
#define AXP2101_PKEY_LONG  (1 << 2)
#define AXP2101_PKEY_SHORT (1 << 3)
#define PCF85063_ADDR 0x51
#define PCF85063_SECONDS_REG 0x04

#define LCD_HOST    SPI2_HOST
#define LCD_H_RES   240
#define LCD_V_RES   284
#define MOUNT_POINT "/sdcard"
#define CLIPS_DIR   MOUNT_POINT "/clips"   /* recordings live here; root keeps only wifi.txt */

/* ---- audio (ES7210 via I2S TDM) ---- */
#define I2C_PORT        I2C_NUM_0
#define PIN_I2C_SDA     7
#define PIN_I2C_SCL     8
#define PIN_I2S_MCLK    19
#define PIN_I2S_BCLK    20
#define PIN_I2S_WS      22
#define PIN_I2S_DIN     21

#define SAMPLE_RATE     16000                 /* Whisper-native */
#define SAMPLE_BITS     I2S_DATA_BIT_WIDTH_16BIT
#define CHAN_NUM        2                     /* MIC1 + MIC2 */
#define MIC_SELECTED    (ES7210_SEL_MIC1 | ES7210_SEL_MIC2)
#define MIC_GAIN_DB     37.5f          /* ES7210 analog PGA near max; raise level for quiet capture */
#define MAX_RECORD_SECONDS 15
#define MIN_RECORD_MS      600
#define SCREEN_IDLE_MS     30000       /* blank the LCD after this idle time (retention + power) */
#define PKEY_FAST_POLL_MS   250         /* responsive PWR wake just after the screen blanks */
#define PKEY_SLOW_POLL_MS   1000        /* AXP events latch, so later polling can be much slower */
#define PKEY_FAST_WINDOW_MS 60000
#define AUTO_POWER_OFF_MS   (10 * 60 * 1000) /* true PMIC-off standby after 10 minutes on battery */
#define AUTO_POWER_RETRY_MS 60000
#define WAKE_BATTERY_MS     2000
#define CHARGE_CHECK_MS     5000
#define RECORDING_RESERVE_BYTES (64 * 1024)
#define CLIP_BITMAP_BYTES   125

/* ---- RGB565 colors (byte-swapped at fill time for ST7789-over-SPI) ---- */
#define C_WHITE   0xFFFF
#define C_RED     0xFA69   /* #FF4D4D in RGB565 */
#define C_GREEN   0x5D2B   /* #58A75D in RGB565 */
#define C_AMBER   0xDD65
#define C_BG       0x1082
#define C_PANEL    0x18E3
#define C_TEXT     0xBDBD
#define C_MUTED    0xA514
#define C_DOT      0x39E7
#define C_DARK_RED 0xA145
#define C_BLUE     0x001F
#define C_LINE     0x31A6
#define C_RING     0x0861
#define C_DISABLED 0x5AEB
#define C_CHARCOAL C_BG

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static SemaphoreHandle_t s_lcd_tx_done = NULL;
static esp_lcd_panel_io_handle_t s_touch_io = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_axp = NULL;
static i2c_master_dev_handle_t s_rtc = NULL;
static bool s_sd_ok = false;
static sdmmc_card_t *s_sd_card = NULL;
static bool s_audio_ok = false;
static bool s_touch_ok = false;
static i2s_chan_handle_t s_i2s_rx = NULL;
static esp_codec_dev_handle_t s_codec = NULL;
static bool s_audio_open = false;
static const audio_codec_ctrl_if_t *s_rec_ctrl_if = NULL;
static const audio_codec_data_if_t *s_rec_data_if = NULL;
static const audio_codec_if_t *s_rec_codec_if = NULL;
static int s_active_clip_index = 0;
static bool s_recording_active = false;
static bool s_touch_was_down = false;
static bool s_screen_on = true;
static bool s_upload_active = false;
static int64_t s_last_activity_ms = 0;
static int64_t s_next_pkey_poll_ms = 0;
static int64_t s_screen_sleep_started_ms = 0;
static int64_t s_next_auto_poweroff_attempt_ms = 0;
static int64_t s_ignore_pkey_short_until_ms = 0;
static char s_timer_prev[6] = "";
static int s_wave_x = 0;
static int s_upload_prev_pct = -1;
static bool s_upload_prev_done = false;
static esp_pm_lock_handle_t s_upload_pm_lock = NULL;
static bool s_upload_pm_lock_held = false;
static esp_pm_lock_handle_t s_usb_pm_lock = NULL;
static bool s_usb_pm_lock_held = false;
static int64_t s_next_usb_pm_check_ms = 0;
static int s_home_charge_state = -1;
static int s_home_charge_pct = -1;
static int64_t s_next_charge_check_ms = 0;
static int s_filtered_battery_pct = -1;
static bool s_last_battery_vbus = false;
static bool s_battery_mismatch_logged = false;
static bool s_panel_needs_reinit = true;
static bool s_last_known_vbus = false;
static int64_t s_next_peripheral_retry_ms = 0;
static int64_t s_next_sd_health_ms = 0;
static bool s_trash_pending = false;
static char s_trash_path[96] = "";
static char s_trash_restore_path[96] = "";
static int64_t s_trash_deadline_ms = 0;

typedef enum {
    UI_STATE_STARTING = 0,
    UI_STATE_HOME,
    UI_STATE_RECORDING,
    UI_STATE_SAVING,
    UI_STATE_SNAP_LIST,
    UI_STATE_CONNECTING,
    UI_STATE_UPLOADING,
    UI_STATE_ERROR,
    UI_STATE_SLEEP,
} device_ui_state_t;

static device_ui_state_t s_ui_state = UI_STATE_STARTING;

typedef struct {
    bool valid;
    bool present;
    bool vbus;
    bool charging;
    bool charge_done;
    int percent;
    int millivolts;
    uint8_t charge_state;
} battery_info_t;

static inline uint16_t sw16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

static int next_index(void);
static int pending_clip_count(void);
static bool parse_clip_index(const char *name, int *index);
static void audio_suspend(void);
static esp_err_t lcd_panel_reinitialize(void);
static void ui_set_state(device_ui_state_t state);

typedef enum {
    TOUCH_ACTION_NONE = 0,
    TOUCH_ACTION_RECORD,
    TOUCH_ACTION_CLIPS,
} touch_action_t;

static void ui_set_state(device_ui_state_t state)
{
    if (s_ui_state == state) return;
    ESP_LOGD(TAG, "UI state %d -> %d", s_ui_state, state);
    s_ui_state = state;
}

/* ---------------- LCD ---------------- */
static bool lcd_color_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *event_data,
                                    void *user_ctx)
{
    (void)panel_io;
    (void)event_data;
    (void)user_ctx;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_lcd_tx_done, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static void lcd_wait_transfers(int count)
{
    for (int i = 0; i < count; i++) {
        if (xSemaphoreTake(s_lcd_tx_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "LCD transfer completion timeout (%d/%d)", i, count);
            break;
        }
    }
}

static void lcd_fill(uint16_t color)
{
    /* Paint in multi-line strips: one SPI transaction per strip instead of one
     * per row cuts a full-screen fill from 284 transactions to ~9, so a screen
     * repaint is fast enough to not read as a flash. */
    enum { FILL_STRIP = 32 };
    static uint16_t buf[LCD_H_RES * FILL_STRIP];
    uint16_t v = sw16(color);
    for (int i = 0; i < LCD_H_RES * FILL_STRIP; i++) buf[i] = v;
    int transfers = 0;
    for (int y = 0; y < LCD_V_RES; y += FILL_STRIP) {
        int h = (LCD_V_RES - y < FILL_STRIP) ? (LCD_V_RES - y) : FILL_STRIP;
        if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + h, buf) == ESP_OK) {
            transfers++;
        }
    }
    lcd_wait_transfers(transfers);
}

/* Normal in-app transitions repaint in place. Physical wake is handled
 * separately and keeps the backlight dark until the first frame is complete. */
static void ui_transition_end(void) { }

static void ui_transition_fill(uint16_t color)
{
    lcd_fill(color);
}

static void lcd_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LCD_H_RES) x1 = LCD_H_RES;
    if (y1 > LCD_V_RES) y1 = LCD_V_RES;
    if (x1 <= x0 || y1 <= y0) return;

    enum { RECT_STRIP = 16 };
    static uint16_t tile[LCD_H_RES * RECT_STRIP];
    uint16_t v = sw16(color);
    int w = x1 - x0;
    int max_pixels = w * RECT_STRIP;
    for (int i = 0; i < max_pixels; i++) tile[i] = v;
    int transfers = 0;
    for (int y = y0; y < y1; y += RECT_STRIP) {
        int h = y1 - y < RECT_STRIP ? y1 - y : RECT_STRIP;
        if (esp_lcd_panel_draw_bitmap(s_panel, x0, y, x1, y + h, tile) == ESP_OK) {
            transfers++;
        }
    }
    lcd_wait_transfers(transfers);
}

static void lcd_disc(int cx, int cy, int r, uint16_t color)
{
    static uint16_t row[LCD_H_RES];
    uint16_t v = sw16(color);
    int transfers = 0;
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= LCD_V_RES) continue;
        int dy = y - cy;
        int dx = 0;
        while (dx * dx + dy * dy <= r * r) dx++;
        int x0 = cx - dx + 1;
        int x1 = cx + dx;
        if (x0 < 0) x0 = 0;
        if (x1 > LCD_H_RES) x1 = LCD_H_RES;
        int w = x1 - x0;
        for (int x = 0; x < w; x++) row[x] = v;
        if (w > 0 && esp_lcd_panel_draw_bitmap(s_panel, x0, y, x1, y + 1, row) == ESP_OK) {
            transfers++;
        }
    }
    lcd_wait_transfers(transfers);
}

static void lcd_circle_outline(int cx, int cy, int r, int thickness, uint16_t color)
{
    lcd_disc(cx, cy, r, color);
    lcd_disc(cx, cy, r - thickness, C_BG);
}

static void lcd_round_rect_fill(int x, int y, int w, int h, int r, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    int max_r = (w < h ? w : h) / 2;
    if (r > max_r) r = max_r;
    if (r < 0) r = 0;
    lcd_rect(x + r, y, x + w - r, y + h, color);
    lcd_rect(x, y + r, x + w, y + h - r, color);
    lcd_disc(x + r, y + r, r, color);
    lcd_disc(x + w - r - 1, y + r, r, color);
    lcd_disc(x + r, y + h - r - 1, r, color);
    lcd_disc(x + w - r - 1, y + h - r - 1, r, color);
}

static void lcd_round_rect_outline(int x, int y, int w, int h, int r, int thickness, uint16_t color, uint16_t fill)
{
    lcd_round_rect_fill(x, y, w, h, r, color);
    lcd_round_rect_fill(x + thickness, y + thickness, w - 2 * thickness, h - 2 * thickness,
                        r - thickness, fill);
}

static void lcd_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    int radius = thickness > 1 ? thickness / 2 : 0;
    if (y0 == y1) {
        if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
        lcd_rect(x0, y0 - radius, x1 + 1, y0 + radius + 1, color);
        return;
    }
    if (x0 == x1) {
        if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
        lcd_rect(x0 - radius, y0, x0 + radius + 1, y1 + 1, color);
        return;
    }

    if (y0 > y1) {
        int tx = x0, ty = y0;
        x0 = x1; y0 = y1; x1 = tx; y1 = ty;
    }
    int dy = y1 - y0;
    for (int y = y0; y <= y1; y++) {
        int x = x0 + (x1 - x0) * (y - y0) / dy;
        lcd_rect(x - radius, y, x + radius + 1, y + 1, color);
    }
}

static uint8_t glyph_row(char ch, int row)
{
    static const uint8_t blank[7] = {0,0,0,0,0,0,0};
    const uint8_t *g = blank;
    static const uint8_t A[7]={14,17,17,31,17,17,17}, C[7]={14,17,16,16,16,17,14};
    static const uint8_t D[7]={30,17,17,17,17,17,30}, E[7]={31,16,16,30,16,16,31};
    static const uint8_t F[7]={31,16,16,30,16,16,16};
    static const uint8_t G[7]={14,17,16,23,17,17,15}, I[7]={14,4,4,4,4,4,14};
    static const uint8_t L[7]={16,16,16,16,16,16,31}, N[7]={17,25,21,19,17,17,17};
    static const uint8_t O[7]={14,17,17,17,17,17,14}, P[7]={30,17,17,30,16,16,16};
    static const uint8_t R[7]={30,17,17,30,20,18,17}, S[7]={15,16,16,14,1,1,30};
    static const uint8_t T[7]={31,4,4,4,4,4,4}, U[7]={17,17,17,17,17,17,14};
    static const uint8_t V[7]={17,17,17,17,17,10,4}, W[7]={17,17,17,21,21,21,10};
    static const uint8_t zero[7]={14,17,19,21,25,17,14}, one[7]={4,12,4,4,4,4,14};
    static const uint8_t two[7]={14,17,1,2,4,8,31}, three[7]={30,1,1,14,1,1,30};
    static const uint8_t four[7]={2,6,10,18,31,2,2}, five[7]={31,16,16,30,1,1,30};
    static const uint8_t six[7]={14,16,16,30,17,17,14}, seven[7]={31,1,2,4,8,8,8};
    static const uint8_t eight[7]={14,17,17,14,17,17,14}, nine[7]={14,17,17,15,1,1,14};
    static const uint8_t dot[7]={0,0,0,0,0,12,12}, dash[7]={0,0,0,31,0,0,0};
    static const uint8_t lbr[7]={14,8,8,8,8,8,14}, rbr[7]={14,2,2,2,2,2,14};
    switch (ch) {
    case 'A': g=A; break; case 'C': g=C; break; case 'D': g=D; break; case 'E': g=E; break;
    case 'F': g=F; break; case 'G': g=G; break; case 'I': g=I; break; case 'L': g=L; break; case 'N': g=N; break;
    case 'O': g=O; break; case 'P': g=P; break; case 'R': g=R; break; case 'S': g=S; break;
    case 'T': g=T; break; case 'U': g=U; break; case 'V': g=V; break; case 'W': g=W; break;
    case '0': g=zero; break; case '1': g=one; break; case '2': g=two; break; case '3': g=three; break;
    case '4': g=four; break; case '5': g=five; break; case '6': g=six; break; case '7': g=seven; break;
    case '8': g=eight; break; case '9': g=nine; break; case '.': g=dot; break; case '-': g=dash; break;
    case '[': g=lbr; break; case ']': g=rbr; break;
    }
    return g[row];
}

static void draw_text(int x, int y, const char *s, int scale, uint16_t color)
{
    for (const char *p = s; *p; p++, x += 6 * scale) {
        if (*p == ' ') continue;
        for (int row = 0; row < 7; row++) {
            uint8_t bits = glyph_row(*p, row);
            for (int col = 0; col < 5; col++) {
                if (bits & (1 << (4 - col))) {
                    lcd_rect(x + col * scale, y + row * scale,
                             x + (col + 1) * scale, y + (row + 1) * scale, color);
                }
            }
        }
    }
}

static const ui_glyph_t *font_lookup(const ui_glyph_t *glyphs, int count, char ch)
{
    for (int i = 0; i < count; i++) {
        if (glyphs[i].ch == ch) return &glyphs[i];
    }
    return NULL;
}

static int font_text_width(const ui_glyph_t *glyphs, int count, const char *s)
{
    int width = 0;
    for (const char *p = s; *p; p++) {
        const ui_glyph_t *g = font_lookup(glyphs, count, *p);
        if (g) width += g->advance;
    }
    return width;
}

static void draw_font_text(const ui_glyph_t *glyphs, int count, const uint8_t *bitmap,
                           int x, int y, const char *s, uint16_t color)
{
    for (const char *p = s; *p; p++) {
        const ui_glyph_t *g = font_lookup(glyphs, count, *p);
        if (!g) continue;
        int row_bytes = (g->w + 7) / 8;
        for (int yy = 0; yy < g->h; yy++) {
            int xx = 0;
            while (xx < g->w) {
                uint8_t byte = bitmap[g->offset + yy * row_bytes + xx / 8];
                if (!(byte & (1 << (7 - (xx % 8))))) { xx++; continue; }
                int run = xx;
                while (xx < g->w) {
                    byte = bitmap[g->offset + yy * row_bytes + xx / 8];
                    if (!(byte & (1 << (7 - (xx % 8))))) break;
                    xx++;
                }
                lcd_rect(x + run + g->xoff, y + yy + g->yoff,
                         x + xx + g->xoff, y + yy + g->yoff + 1, color);
            }
        }
        x += g->advance;
    }
}

static int label_width(const char *s)
{
    return font_text_width(ui_label_glyphs, UI_LABEL_GLYPH_COUNT, s);
}

static void label_ink_size(const char *s, int *width, int *height)
{
    int pen_x = 0;
    int right = 0;
    int bottom = 0;
    for (const char *p = s; *p; p++) {
        const ui_glyph_t *g = font_lookup(ui_label_glyphs, UI_LABEL_GLYPH_COUNT, *p);
        if (!g) continue;
        if (g->w > 0 && pen_x + g->xoff + g->w > right) {
            right = pen_x + g->xoff + g->w;
        }
        if (g->h > 0 && g->yoff + g->h > bottom) {
            bottom = g->yoff + g->h;
        }
        pen_x += g->advance;
    }
    *width = right;
    *height = bottom;
}

static void draw_label_text(int x, int y, const char *s, uint16_t color)
{
    draw_font_text(ui_label_glyphs, UI_LABEL_GLYPH_COUNT, ui_label_bitmap, x, y, s, color);
}

static void draw_label_centered(int x, int y, int w, int h, const char *s, uint16_t color)
{
    int text_w = 0;
    int text_h = 0;
    label_ink_size(s, &text_w, &text_h);
    draw_label_text(x + (w - text_w) / 2, y + (h - text_h) / 2, s, color);
}

static int big_width(const char *s)
{
    return font_text_width(ui_big_glyphs, UI_BIG_GLYPH_COUNT, s);
}

static void draw_big_text(int x, int y, const char *s, uint16_t color)
{
    draw_font_text(ui_big_glyphs, UI_BIG_GLYPH_COUNT, ui_big_bitmap, x, y, s, color);
}

static void draw_big_char_centered(int x, int y, int w, char ch, uint16_t color)
{
    char text[2] = {ch, '\0'};
    const ui_glyph_t *g = font_lookup(ui_big_glyphs, UI_BIG_GLYPH_COUNT, ch);
    int dx = g ? (w - g->w) / 2 : 0;
    draw_big_text(x + dx, y, text, color);
}

static void draw_status(int y, uint16_t dot_color, const char *label)
{
    lcd_rect(0, y - 8, 170, y + 24, C_BG);
    lcd_disc(27, y + 7, 6, dot_color);
    draw_label_text(38, y - 2, label, C_TEXT);
}

static void draw_clip_pill(int clip_index)
{
    const int x = 174, y = 228, w = 44, h = 28;
    char buf[4];
    snprintf(buf, sizeof(buf), "%02d", clip_index % 100);
    lcd_round_rect_fill(x, y, w, h, h / 2, C_TEXT);
    draw_label_centered(x, y, w, h, buf, C_BG);
}

static void format_time_value(uint32_t elapsed_ms, char out[6])
{
    uint32_t total = elapsed_ms / 1000;
    int mm = total / 60;
    int ss = total % 60;
    if (mm > 99) mm = 99;
    snprintf(out, 6, "%02d:%02d", mm, ss);
}

static void draw_face(void)
{
    draw_text(45, 122, "O", 5, C_TEXT);
    draw_text(166, 122, "O", 5, C_TEXT);
    lcd_line(62, 113, 70, 131, 3, C_TEXT);
    lcd_line(185, 113, 177, 131, 3, C_TEXT);
    lcd_rect(106, 164, 135, 167, C_TEXT);
}

static void draw_pill_button(int y, const char *label, bool enabled)
{
    const int h = 44;
    int text_w = 0, text_h = 0;
    label_ink_size(label, &text_w, &text_h);
    int w = text_w + 44;
    int x = (LCD_H_RES - w) / 2;
    uint16_t color = enabled ? C_TEXT : C_DISABLED;
    lcd_round_rect_outline(x, y, w, h, h / 2, 1, color, C_BG);
    draw_label_centered(x, y, w, h, label, color);
}

static void draw_pill_pressed(int y, const char *label)
{
    const int h = 44;
    int text_w = 0, text_h = 0;
    label_ink_size(label, &text_w, &text_h);
    int w = text_w + 44;
    int x = (LCD_H_RES - w) / 2;
    lcd_round_rect_fill(x, y, w, h, h / 2, C_TEXT);
    draw_label_centered(x, y, w, h, label, C_BG);
    vTaskDelay(pdMS_TO_TICKS(45));
}

static void draw_clips_pill(int pending)
{
    char label[20];
    snprintf(label, sizeof(label), "SNAPS [%d]", pending);
    draw_pill_button(32, label, true);
}

static void draw_clips_pill_pressed(int pending)
{
    char label[20];
    snprintf(label, sizeof(label), "SNAPS [%d]", pending);
    draw_pill_pressed(32, label);
}

static void draw_upload_pill(int pending)
{
    char label[20];
    snprintf(label, sizeof(label), "UPLOAD [%d]", pending);
    draw_pill_button(22, label, pending > 0);
}

static void draw_upload_pill_pressed(int pending)
{
    char label[20];
    snprintf(label, sizeof(label), "UPLOAD [%d]", pending);
    draw_pill_pressed(22, label);
}

static void ui_show_battery(int pct)
{
    ui_transition_fill(C_BG);
    int x = 60, y = 114, w = 120, h = 62;
    lcd_round_rect_outline(x, y, w, h, 16, 3, C_TEXT, C_BG);
    lcd_round_rect_fill(x + w + 4, y + 22, 7, 18, 2, C_TEXT);
    int fill_w = (w - 10) * pct / 100;
    if (fill_w > w - 10) fill_w = w - 10;
    if (fill_w > 0) {
        uint16_t fill = pct < 15 ? C_RED : C_AMBER;
        lcd_round_rect_fill(x + 5, y + 5, fill_w, h - 10, 11, fill);
    }
    ui_transition_end();
}

static void ui_show_power_message(const char *label, uint16_t color)
{
    const char *name = "WORDSNAP";
    ui_transition_fill(C_BG);
    draw_big_text((LCD_H_RES - big_width(name)) / 2, 105, name, C_TEXT);
    draw_status(230, color, label);
    ui_transition_end();
}

static esp_err_t shared_i2c_init(void)
{
    if (s_i2c_bus) return ESP_OK;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

static bool axp_ready(void)
{
    if (!s_i2c_bus) return false;
    if (s_axp) return true;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &cfg, &s_axp) == ESP_OK;
}

static bool axp_read_reg(uint8_t reg, uint8_t *value)
{
    return axp_ready() &&
           i2c_master_transmit_receive(s_axp, &reg, 1, value, 1, 100) == ESP_OK;
}

static bool axp_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return axp_ready() && i2c_master_transmit(s_axp, data, sizeof(data), 100) == ESP_OK;
}

static bool axp_update_reg(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t value = 0;
    return axp_read_reg(reg, &value) &&
           axp_write_reg(reg, (value & (uint8_t)~clear_mask) | set_mask);
}

static uint8_t bcd_encode(int value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static int bcd_decode(uint8_t value)
{
    return ((value >> 4) & 0x0F) * 10 + (value & 0x0F);
}

static bool rtc_ready(void)
{
    if (!s_i2c_bus) return false;
    if (s_rtc) return true;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &cfg, &s_rtc) == ESP_OK;
}

static bool rtc_set_utc(time_t epoch)
{
    if (!rtc_ready() || epoch < 1704067200) return false;
    struct tm utc = {0};
    gmtime_r(&epoch, &utc);
    uint8_t data[] = {
        PCF85063_SECONDS_REG,
        bcd_encode(utc.tm_sec), bcd_encode(utc.tm_min), bcd_encode(utc.tm_hour),
        bcd_encode(utc.tm_mday), bcd_encode(utc.tm_wday),
        bcd_encode(utc.tm_mon + 1), bcd_encode((utc.tm_year + 1900) % 100),
    };
    return i2c_master_transmit(s_rtc, data, sizeof(data), 100) == ESP_OK;
}

static bool rtc_restore_system_time(void)
{
    if (!rtc_ready()) return false;
    uint8_t reg = PCF85063_SECONDS_REG;
    uint8_t data[7] = {0};
    if (i2c_master_transmit_receive(s_rtc, &reg, 1, data, sizeof(data), 100) != ESP_OK) return false;
    if (data[0] & 0x80) return false;
    struct tm utc = {
        .tm_sec = bcd_decode(data[0] & 0x7F),
        .tm_min = bcd_decode(data[1] & 0x7F),
        .tm_hour = bcd_decode(data[2] & 0x3F),
        .tm_mday = bcd_decode(data[3] & 0x3F),
        .tm_wday = bcd_decode(data[4] & 0x07),
        .tm_mon = bcd_decode(data[5] & 0x1F) - 1,
        .tm_year = bcd_decode(data[6]) + 100,
        .tm_isdst = 0,
    };
    if (utc.tm_year < 124 || utc.tm_mon < 0 || utc.tm_mon > 11 || utc.tm_mday < 1 || utc.tm_mday > 31) {
        return false;
    }
    time_t epoch = mktime(&utc);
    if (epoch < 1704067200) return false;
    struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "system time restored from PCF85063");
    return true;
}

static void axp_init_battery(void)
{
    bool ok = true;
    /* A fully depleted battery can reset retained PMIC state. Restore the board
     * rails explicitly so the USB-to-battery handoff uses Waveshare's BSP
     * voltages instead of whatever register values happened to survive. */
    ok &= axp_write_reg(AXP2101_DC1_VOLTAGE, 0x12); /* DC1: 3.3 V */
    ok &= axp_update_reg(AXP2101_ALDO1_VOLTAGE, 0x1F, 0x1C); /* ALDO1: 3.3 V */
    ok &= axp_update_reg(AXP2101_ALDO2_VOLTAGE, 0x1F, 0x1C); /* ALDO2: 3.3 V */
    ok &= axp_update_reg(AXP2101_DC_ONOFF, 0, (1 << 0));
    ok &= axp_update_reg(AXP2101_LDO_ONOFF0, 0, (1 << 0) | (1 << 1));

    /* This board has no battery thermistor on TS. Leaving TS measurement active
     * can inhibit charging, so match the Waveshare reference configuration. */
    ok &= axp_update_reg(AXP2101_TS_PIN_CTRL, 0x0F, 0x10);
    ok &= axp_update_reg(AXP2101_ADC_CTRL, (1 << 1), (1 << 0));
    ok &= axp_update_reg(AXP2101_BAT_DETECT, 0, (1 << 0));

    /* Conservative settings for the installed 400 mAh single-cell LiPo:
     * 50 mA precharge, 200 mA constant current, 25 mA termination, 4.20 V. */
    ok &= axp_update_reg(AXP2101_PRECHARGE, 0x03, 0x02);
    ok &= axp_update_reg(AXP2101_CHARGE_CURRENT, 0x1F, 0x08);
    ok &= axp_update_reg(AXP2101_TERMINATION, 0x1F, 0x11);
    ok &= axp_update_reg(AXP2101_TARGET_VOLTAGE, 0x07, 0x03);

    if (ok) ESP_LOGI(TAG, "AXP2101 3.3 V rails + battery ADC + 200 mA charger configured");
    else ESP_LOGW(TAG, "AXP2101 battery configuration incomplete");
}

static void axp_enable_pkey_events(void)
{
    uint8_t enabled = 0;
    const uint8_t mask = AXP2101_PKEY_LONG | AXP2101_PKEY_SHORT;
    if (!axp_read_reg(AXP2101_INTEN2, &enabled) ||
        !axp_write_reg(AXP2101_INTEN2, enabled | mask) ||
        !axp_write_reg(AXP2101_INTSTS2, mask)) {
        ESP_LOGW(TAG, "could not enable PWR button events from AXP2101");
        return;
    }
    ESP_LOGI(TAG, "AXP2101 PWR short/long button events enabled");
}

static uint8_t axp_take_pkey_events(void)
{
    uint8_t status = 0;
    const uint8_t mask = AXP2101_PKEY_LONG | AXP2101_PKEY_SHORT;
    if (!axp_read_reg(AXP2101_INTSTS2, &status)) return 0;
    status &= mask;
    if (status && !axp_write_reg(AXP2101_INTSTS2, status)) {
        ESP_LOGW(TAG, "could not clear AXP2101 PWR event");
    }
    return status;
}

static bool axp_shutdown(void)
{
    uint8_t config = 0;
    return axp_read_reg(AXP2101_COMMON_CONFIG, &config) &&
           axp_write_reg(AXP2101_COMMON_CONFIG, config | (1 << 0));
}

static int battery_percent_from_voltage(int millivolts)
{
    static const int mv[]  = {3300, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200};
    static const int pct[] = {   0,    5,   10,   20,   30,   45,   60,   70,   85,   95,  100};
    if (millivolts <= mv[0]) return 0;
    for (size_t i = 1; i < sizeof(mv) / sizeof(mv[0]); i++) {
        if (millivolts <= mv[i]) {
            return pct[i - 1] +
                   (millivolts - mv[i - 1]) * (pct[i] - pct[i - 1]) / (mv[i] - mv[i - 1]);
        }
    }
    return 100;
}

static int battery_filter_percent(int candidate, bool vbus)
{
    if (candidate < 0 || candidate > 100) return -1;
    if (s_filtered_battery_pct < 0) {
        s_filtered_battery_pct = candidate;
    } else {
        int delta = candidate - s_filtered_battery_pct;
        int max_step = (vbus == s_last_battery_vbus) ? 5 : 10;
        if (delta > max_step) delta = max_step;
        if (delta < -max_step) delta = -max_step;
        s_filtered_battery_pct += delta;
    }
    s_last_battery_vbus = vbus;
    return s_filtered_battery_pct;
}

static battery_info_t battery_read(void)
{
    battery_info_t info = {.percent = -1, .millivolts = -1};
    uint8_t status1 = 0, status2 = 0;
    if (!axp_read_reg(AXP2101_STATUS1, &status1) ||
        !axp_read_reg(AXP2101_STATUS2, &status2)) return info;

    info.valid = true;
    info.present = (status1 & (1 << 3)) != 0;
    info.vbus = (status1 & (1 << 5)) != 0;
    info.charge_state = status2 & 0x07;
    info.charging = info.present && info.vbus &&
                    (((status2 >> 5) == 0x01) ||
                     (info.charge_state >= 1 && info.charge_state <= 3));
    info.charge_done = info.present && info.vbus && info.charge_state == 4;
    if (!info.present) return info;

    uint8_t high = 0, low = 0, raw_pct = 0;
    bool voltage_valid = axp_read_reg(AXP2101_BAT_V_H, &high) &&
                         axp_read_reg(AXP2101_BAT_V_L, &low);
    if (voltage_valid) {
        info.millivolts = ((high & 0x1F) << 8) | low;
        voltage_valid = info.millivolts >= 2800 && info.millivolts <= 4500;
        if (!voltage_valid) info.millivolts = -1;
    }

    bool gauge_valid = axp_read_reg(AXP2101_BAT_PERCENT, &raw_pct) && raw_pct <= 100;
    int voltage_pct = voltage_valid ? battery_percent_from_voltage(info.millivolts) : -1;
    int candidate = gauge_valid ? raw_pct : voltage_pct;
    bool mismatch = gauge_valid && voltage_valid && abs((int)raw_pct - voltage_pct) > 35;
    if (mismatch) {
        /* Charge voltage is elevated by the charger and is not a reliable SoC
         * estimate. Use voltage to reject a wild gauge value only on battery. */
        if (!info.vbus) candidate = voltage_pct;
        if (!s_battery_mismatch_logged) {
            ESP_LOGW(TAG, "battery gauge mismatch: raw=%u voltage=%dmV (~%d%%), using %s",
                     raw_pct, info.millivolts, voltage_pct, info.vbus ? "gauge" : "voltage");
            s_battery_mismatch_logged = true;
        }
    } else {
        s_battery_mismatch_logged = false;
    }
    info.percent = battery_filter_percent(candidate, info.vbus);
    return info;
}

static void usb_pm_poll(int64_t now_ms, bool force)
{
    if (!s_usb_pm_lock || (!force && now_ms < s_next_usb_pm_check_ms)) return;
    s_next_usb_pm_check_ms = now_ms + 1000;

    uint8_t status1 = 0;
    if (!axp_read_reg(AXP2101_STATUS1, &status1)) return;
    bool vbus = (status1 & (1 << 5)) != 0;
    if (vbus != s_last_known_vbus) {
        s_last_known_vbus = vbus;
        s_panel_needs_reinit = true;
    }
    if (vbus == s_usb_pm_lock_held) return;

    esp_err_t err = vbus ? esp_pm_lock_acquire(s_usb_pm_lock)
                         : esp_pm_lock_release(s_usb_pm_lock);
    if (err == ESP_OK) {
        s_usb_pm_lock_held = vbus;
        ESP_LOGI(TAG, "USB power %s; automatic light sleep %s",
                 vbus ? "connected" : "removed", vbus ? "paused" : "available");
    } else {
        ESP_LOGW(TAG, "USB PM lock update failed: %s", esp_err_to_name(err));
    }
}

static void draw_charge_bolt_at(int ox, int oy, uint16_t color)
{
    static const uint8_t px[] = {11, 0, 7, 3, 19, 12};
    static const uint8_t py[] = {0, 17, 17, 31, 11, 11};
    const int n = 6;
    for (int yy = 0; yy <= 31; yy++) {
        int hits[6];
        int hit_count = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            if ((py[i] <= yy && py[j] > yy) || (py[j] <= yy && py[i] > yy)) {
                hits[hit_count++] = px[i] + (yy - py[i]) * (px[j] - px[i]) / (py[j] - py[i]);
            }
        }
        for (int i = 0; i + 1 < hit_count; i += 2) {
            if (hits[i] > hits[i + 1]) {
                int tmp = hits[i];
                hits[i] = hits[i + 1];
                hits[i + 1] = tmp;
            }
            lcd_rect(ox + hits[i], oy + yy, ox + hits[i + 1] + 1, oy + yy + 1, color);
        }
    }
}

static void draw_small_percent(int x, int y, uint16_t color)
{
    lcd_disc(x + 2, y + 3, 2, color);
    lcd_disc(x + 10, y + 13, 2, color);
    lcd_line(x + 12, y + 0, x + 0, y + 16, 2, color);
}

static int percent_width(void)
{
    return 15;
}

static void draw_label_with_percent(int x, int y, const char *text, uint16_t color)
{
    size_t len = strlen(text);
    if (len > 0 && text[len - 1] == '%') {
        char buf[24];
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, text, len - 1);
        buf[len - 1] = '\0';
        draw_label_text(x, y, buf, color);
        draw_small_percent(x + label_width(buf) + 2, y + 1, color);
    } else {
        draw_label_text(x, y, text, color);
    }
}

static int label_with_percent_width(const char *text)
{
    size_t len = strlen(text);
    if (len > 0 && text[len - 1] == '%') {
        char buf[24];
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, text, len - 1);
        buf[len - 1] = '\0';
        return label_width(buf) + 2 + percent_width();
    }
    return label_width(text);
}

static void ui_home_charge_indicator(int state, int pct)
{
    lcd_rect(0, 248, LCD_H_RES, 276, C_BG);
    if (state == 1) {
        char pct_text[8];
        snprintf(pct_text, sizeof(pct_text), "%d%%", pct);
        int bolt_text_gap = 12;
        int row_w = 19 + bolt_text_gap + label_width("CHARGING") + 8 + label_with_percent_width(pct_text);
        int x = (LCD_H_RES - row_w) / 2;
        draw_charge_bolt_at(x, 250, C_AMBER);
        draw_label_text(x + 19 + bolt_text_gap, 256, "CHARGING", C_AMBER);
        draw_label_with_percent(x + 19 + bolt_text_gap + label_width("CHARGING") + 8, 256, pct_text, C_AMBER);
    } else if (state == 2) {
        char pct_text[8];
        snprintf(pct_text, sizeof(pct_text), "%d%%", pct);
        int bolt_text_gap = 12;
        int row_w = 19 + bolt_text_gap + label_with_percent_width(pct_text);
        int x = (LCD_H_RES - row_w) / 2;
        draw_charge_bolt_at(x, 250, C_GREEN);
        draw_label_with_percent(x + 19 + bolt_text_gap, 256, pct_text, C_GREEN);
    }
    s_home_charge_state = state;
    s_home_charge_pct = pct;
}

static void ui_home_poll_charge_indicator(int64_t now_ms)
{
    if (now_ms < s_next_charge_check_ms) return;
    int state = 0;
    int pct = -1;
    battery_info_t battery = battery_read();
    if (battery.valid && battery.present && battery.vbus && battery.percent >= 0) {
        pct = battery.percent;
        if (battery.charging) state = 1;
        else if (battery.charge_done) state = 2;
    }
    if (state != s_home_charge_state || (state != 0 && pct != s_home_charge_pct)) {
        ui_home_charge_indicator(state, pct);
    }
    s_next_charge_check_ms = now_ms + CHARGE_CHECK_MS;
}

static void ui_show_home(void)
{
    ui_set_state(UI_STATE_HOME);
    ui_transition_fill(C_BG);
    draw_clips_pill(pending_clip_count());
    lcd_disc(120, 180, 67, C_RING);
    lcd_circle_outline(120, 180, 59, 3, C_PANEL);
    lcd_disc(120, 180, 47, C_RED);
    s_home_charge_state = -1;
    s_home_charge_pct = -1;
    ui_home_poll_charge_indicator(0);
    s_next_charge_check_ms = (esp_timer_get_time() / 1000) + CHARGE_CHECK_MS;
    ui_transition_end();

    ESP_LOGI(TAG, "UI home: pending=%d", pending_clip_count());
}

static void ui_show_home_after_recording(void)
{
    ui_set_state(UI_STATE_HOME);
    /* Preserve visible content while rebuilding Home. A full-screen background
     * fill exposes a blank frame because the button and type take longer to
     * draw; replacing one section at a time keeps the transition continuous. */
    lcd_rect(0, 0, LCD_H_RES, 116, C_BG);
    draw_clips_pill(pending_clip_count());

    lcd_rect(0, 200, LCD_H_RES, LCD_V_RES, C_BG);
    lcd_rect(0, 116, LCD_H_RES, 200, C_BG);
    lcd_disc(120, 180, 67, C_RING);
    lcd_circle_outline(120, 180, 59, 3, C_PANEL);
    lcd_disc(120, 180, 47, C_RED);

    s_home_charge_state = -1;
    s_home_charge_pct = -1;
    ui_home_poll_charge_indicator(0);
    s_next_charge_check_ms = (esp_timer_get_time() / 1000) + CHARGE_CHECK_MS;

    ESP_LOGI(TAG, "UI home after recording: pending=%d", pending_clip_count());
}

static void ui_home_record_pressed(void)
{
    lcd_disc(120, 180, 67, C_RING);
    lcd_circle_outline(120, 180, 59, 3, C_PANEL);
    lcd_disc(120, 180, 42, C_RED);
    vTaskDelay(pdMS_TO_TICKS(35));
    lcd_disc(120, 180, 50, C_RED);
    vTaskDelay(pdMS_TO_TICKS(25));
}

static void draw_x_icon(int cx, int cy, uint16_t color)
{
    lcd_line(cx - 7, cy - 7, cx + 7, cy + 7, 3, color);
    lcd_line(cx + 7, cy - 7, cx - 7, cy + 7, 3, color);
}

static void draw_icon_press_flash(int cx, int cy, bool destructive)
{
    uint16_t color = destructive ? C_RED : C_TEXT;
    lcd_circle_outline(cx, cy, 18, 2, color);
    vTaskDelay(pdMS_TO_TICKS(35));
}

static void draw_up_icon(int cx, int cy, uint16_t color)
{
    lcd_line(cx - 8, cy + 5, cx, cy - 5, 3, color);
    lcd_line(cx, cy - 5, cx + 8, cy + 5, 3, color);
}

static void draw_down_icon(int cx, int cy, uint16_t color)
{
    lcd_line(cx - 8, cy - 5, cx, cy + 5, 3, color);
    lcd_line(cx, cy + 5, cx + 8, cy - 5, 3, color);
}

static void draw_icon_button(int x, int y, int w, int h, bool enabled, bool down)
{
    uint16_t color = enabled ? C_TEXT : C_DISABLED;
    lcd_round_rect_outline(x, y, w, h, h / 2, 1, color, C_BG);
    if (down) draw_down_icon(x + w / 2, y + h / 2, color);
    else draw_up_icon(x + w / 2, y + h / 2, color);
}

static void draw_back_button(bool pressed)
{
    const char *label = s_trash_pending ? "UNDO" : "BACK";
    uint16_t color = s_trash_pending ? C_AMBER : C_TEXT;
    int x = 78, y = 250, w = 84, h = 30;
    if (pressed) {
        lcd_round_rect_fill(x, y, w, h, h / 2, color);
        draw_label_centered(x, y, w, h, label, C_BG);
        vTaskDelay(pdMS_TO_TICKS(45));
    } else {
        lcd_round_rect_outline(x, y, w, h, h / 2, 1, color, C_BG);
        draw_label_centered(x, y, w, h, label, color);
    }
}

static void ui_show_clip_menu(const int *indices, int visible, int total, int offset)
{
    ui_set_state(UI_STATE_SNAP_LIST);
    ui_transition_fill(C_BG);
    draw_upload_pill(total);

    if (total == 0) {
        draw_label_text((LCD_H_RES - label_width("NO SNAPS")) / 2, 132, "NO SNAPS", C_DISABLED);
    }

    for (int i = 0; i < visible; i++) {
        int y = 70 + i * 64;
        char label[4];
        snprintf(label, sizeof(label), "%03d", indices[i]);
        lcd_rect(18, y + 58, 222, y + 59, C_LINE);
        draw_label_centered(30, y, 48, 58, label, C_TEXT);
        draw_x_icon(210, y + 29, C_RED);
    }

    bool can_up = offset > 0;
    bool can_down = offset + visible < total;
    draw_icon_button(20, 250, 44, 30, can_up, false);
    draw_back_button(false);
    draw_icon_button(176, 250, 44, 30, can_down, true);
    ui_transition_end();
}

static void ui_recording_enter_animation(void)
{
    const int cx = 120;
    const int cy = 180;
    const int radii[] = {51, 59, 68};

    for (int i = 0; i < 3; i++) {
        lcd_circle_outline(cx, cy, radii[i], 3, C_RED);
        vTaskDelay(pdMS_TO_TICKS(22));
    }

    /* A fast scanline wipe turns the idle record button into the recording screen. */
    for (int y = 0; y < LCD_V_RES; y += 18) {
        lcd_rect(0, y, LCD_H_RES, y + 18, C_BG);
        if (y >= 180) lcd_rect(0, 198, LCD_H_RES, 200, C_LINE);
        vTaskDelay(pdMS_TO_TICKS(4));
    }

    for (int x = 0; x <= LCD_H_RES / 2; x += 30) {
        lcd_rect(LCD_H_RES / 2 - x, 198, LCD_H_RES / 2 + x, 200, C_LINE);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void ui_recording_draw_static(bool clear_screen)
{
    ui_set_state(UI_STATE_RECORDING);
    if (clear_screen) lcd_fill(C_BG);
    lcd_rect(0, 198, LCD_H_RES, 200, C_LINE);
    draw_status(231, C_RED, "REC");
    draw_clip_pill(s_active_clip_index);
}

static void ui_recording_update_time(uint32_t elapsed_ms)
{
    static const int x[5] = {15, 59, 103, 121, 165};
    static const int w[5] = {42, 42, 18, 42, 42};
    char now[6];
    format_time_value(elapsed_ms, now);

    for (int i = 0; i < 5; i++) {
        if (s_timer_prev[0] != '\0' && s_timer_prev[i] == now[i]) continue;
        lcd_rect(x[i] - 2, 24, x[i] + w[i] + 2, 112, C_BG);
        draw_big_char_centered(x[i], 33, w[i], now[i], C_TEXT);
    }
    memcpy(s_timer_prev, now, sizeof(s_timer_prev));
}

static void ui_recording_update_levels(uint8_t level)
{
    const int left = 0;
    const int right = LCD_H_RES;
    const int base = 198;
    const int top = 116;
    const int step = 6;
    const int bar_w = 4;
    int x = s_wave_x;
    if (x < left || x + bar_w >= right) x = left;

    /* Erase only the next slice; do not blank the whole waveform at wrap. */
    lcd_rect(x, top, x + step + bar_w, base, C_BG);
    lcd_rect(x, base - 1, x + step + bar_w, base, C_LINE);

    int h = 6 + (level * (base - top - 8)) / 100;
    if (h > base - top) h = base - top;
    if (h < 6) h = 6;
    lcd_rect(x, base - h, x + bar_w, base, C_TEXT);
    s_wave_x += step;
    if (s_wave_x + bar_w >= right) s_wave_x = left;
}

static void ui_show_recording(uint32_t elapsed_ms)
{
    memset(s_timer_prev, 0, sizeof(s_timer_prev));
    s_wave_x = 0;
    ui_recording_enter_animation();
    ui_recording_draw_static(false);
    ui_recording_update_time(elapsed_ms);
    lcd_rect(0, 116, LCD_H_RES, 198, C_BG);
    lcd_rect(0, 197, LCD_H_RES, 198, C_LINE);

    int pct = (int)((elapsed_ms * 100) / (MAX_RECORD_SECONDS * 1000));
    ESP_LOGI(TAG, "UI recording: clip=%03d elapsed=%" PRIu32 "ms pct=%d", s_active_clip_index, elapsed_ms, pct);
}

static void ui_show_saved(uint32_t elapsed_ms)
{
    ui_set_state(UI_STATE_SAVING);
    ui_recording_update_time(elapsed_ms);
    lcd_rect(0, 124, LCD_H_RES, 198, C_BG);
    lcd_rect(0, 198, LCD_H_RES, 200, C_LINE);
    draw_clip_pill(s_active_clip_index);
    for (int radius = 7; radius <= 13; radius += 3) {
        lcd_circle_outline(27, 242, radius, 2, C_GREEN);
        vTaskDelay(pdMS_TO_TICKS(35));
    }
    draw_status(235, C_GREEN, "DONE");
    vTaskDelay(pdMS_TO_TICKS(900));
}

static void ui_show_saving(uint32_t elapsed_ms)
{
    ui_set_state(UI_STATE_SAVING);
    ui_recording_update_time(elapsed_ms);
    lcd_rect(0, 220, LCD_H_RES, LCD_V_RES, C_BG);
    draw_status(235, C_AMBER, "SAVING");
    draw_clip_pill(s_active_clip_index);
}

static void draw_wifi_icon_stage(int stage)
{
    static const int outer[][2] = {
        {58, 124}, {78, 107}, {101, 98}, {120, 96}, {139, 98}, {162, 107}, {182, 124},
    };
    static const int middle[][2] = {
        {83, 146}, {98, 134}, {112, 129}, {120, 128}, {128, 129}, {142, 134}, {157, 146},
    };
    static const int inner[][2] = {
        {100, 164}, {110, 156}, {120, 153}, {130, 156}, {140, 164},
    };
    lcd_rect(48, 86, 192, 192, C_BG);
    uint16_t outer_color = stage >= 3 ? C_TEXT : C_DISABLED;
    uint16_t middle_color = stage >= 2 ? C_TEXT : C_DISABLED;
    uint16_t inner_color = stage >= 1 ? C_TEXT : C_DISABLED;
    for (int i = 0; i < (int)(sizeof(outer) / sizeof(outer[0])) - 1; i++) {
        lcd_line(outer[i][0], outer[i][1], outer[i + 1][0], outer[i + 1][1], 6, outer_color);
    }
    for (int i = 0; i < (int)(sizeof(middle) / sizeof(middle[0])) - 1; i++) {
        lcd_line(middle[i][0], middle[i][1], middle[i + 1][0], middle[i + 1][1], 6, middle_color);
    }
    for (int i = 0; i < (int)(sizeof(inner) / sizeof(inner[0])) - 1; i++) {
        lcd_line(inner[i][0], inner[i][1], inner[i + 1][0], inner[i + 1][1], 6, inner_color);
    }
    lcd_disc(120, 179, 8, inner_color);
}

static void ui_show_connecting(void)
{
    ui_set_state(UI_STATE_CONNECTING);
    ui_transition_fill(C_BG);
    draw_wifi_icon_stage(0);
    draw_status(230, C_AMBER, "CONNECTING...");
    ui_transition_end();
    s_upload_prev_pct = -1;
    s_upload_prev_done = false;
}

static void ui_show_upload_result(int sent, int retry)
{
    ui_set_state(retry > 0 ? UI_STATE_ERROR : UI_STATE_UPLOADING);
    ui_transition_fill(C_BG);
    char total[8];
    snprintf(total, sizeof(total), "%d", sent);
    draw_big_text((LCD_H_RES - big_width(total)) / 2, 102, total, C_TEXT);
    char sent_label[16];
    snprintf(sent_label, sizeof(sent_label), "%d SENT", sent);
    draw_label_text((LCD_H_RES - label_width(sent_label)) / 2, 184, sent_label, C_TEXT);
    if (retry > 0) {
        char label[20];
        snprintf(label, sizeof(label), "%d RETRY", retry);
        draw_status(230, C_AMBER, label);
    } else {
        draw_status(230, C_GREEN, "DONE");
    }
    ui_transition_end();
}

static void ui_draw_upload_percent(int pct)
{
    lcd_rect(20, 96, 220, 170, C_BG);
    char label[8];
    snprintf(label, sizeof(label), "%d%%", pct);
    draw_big_text((LCD_H_RES - big_width(label)) / 2, 104, label, C_TEXT);
}

static void ui_show_uploading(int pct)
{
    ui_set_state(UI_STATE_UPLOADING);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    bool done = pct >= 100;
    if (s_upload_prev_pct < 0) {
        ui_transition_fill(C_BG);
        ui_transition_end();
        ui_draw_upload_percent(pct);
    } else if (s_upload_prev_pct != pct) {
        int start = s_upload_prev_pct;
        int delta = pct - start;
        int distance = abs(delta);
        int frames = distance;
        if (done || frames > 8) frames = 1;
        for (int frame = 1; frame <= frames; frame++) {
            int value = start + (delta * frame) / frames;
            if (frame == frames) value = pct;
            ui_draw_upload_percent(value);
            if (frames > 1) vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (s_upload_prev_done != done || s_upload_prev_pct < 0) {
        draw_status(230, C_GREEN, done ? "DONE" : "UPLOADING...");
    }
    s_upload_prev_pct = pct;
    s_upload_prev_done = done;
}

static void ui_show_error(const char *label)
{
    ui_set_state(UI_STATE_ERROR);
    ui_transition_fill(C_BG);
    draw_face();
    draw_status(230, C_RED, label);
    ui_transition_end();
}

static void keep_screen_awake(void)
{
    s_last_activity_ms = esp_timer_get_time() / 1000;
}

static void upload_busy_begin(void)
{
    s_upload_active = true;
    keep_screen_awake();
    if (s_upload_pm_lock && !s_upload_pm_lock_held) {
        esp_err_t err = esp_pm_lock_acquire(s_upload_pm_lock);
        if (err == ESP_OK) {
            s_upload_pm_lock_held = true;
        } else {
            ESP_LOGW(TAG, "upload PM lock acquire failed: %s", esp_err_to_name(err));
        }
    }
}

static void upload_busy_end(void)
{
    keep_screen_awake();
    if (s_upload_pm_lock && s_upload_pm_lock_held) {
        esp_err_t err = esp_pm_lock_release(s_upload_pm_lock);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "upload PM lock release failed: %s", esp_err_to_name(err));
        }
        s_upload_pm_lock_held = false;
    }
    s_upload_active = false;
}

/* ---------------- Screen power (idle blank to avoid LCD image retention) ---------------- */
static void screen_sleep(void)
{
    bool entering_sleep = s_screen_on;
    gpio_set_level(PIN_LCD_BL, 0);
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, false);
    if (err != ESP_OK) ESP_LOGW(TAG, "panel sleep failed: %s", esp_err_to_name(err));
    s_screen_on = false;
    if (entering_sleep || s_screen_sleep_started_ms == 0) {
        s_screen_sleep_started_ms = esp_timer_get_time() / 1000;
        s_next_auto_poweroff_attempt_ms = s_screen_sleep_started_ms + AUTO_POWER_OFF_MS;
    }
    ui_set_state(UI_STATE_SLEEP);
    ESP_LOGI(TAG, "screen and backlight off");
}

static void screen_wake(void)
{
    gpio_set_level(PIN_LCD_BL, 0);
    esp_err_t err = ESP_OK;
    if (s_panel_needs_reinit) {
        err = lcd_panel_reinitialize();
    } else {
        err = esp_lcd_panel_disp_on_off(s_panel, true);
        if (err != ESP_OK) {
            s_panel_needs_reinit = true;
            err = lcd_panel_reinitialize();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "panel wake failed: %s", esp_err_to_name(err));
        ui_set_state(UI_STATE_ERROR);
        s_screen_on = false;
        return;
    }
    battery_info_t battery = battery_read();
    if (battery.valid && battery.present && !battery.vbus) {
        if (battery.percent >= 0 && battery.percent <= 30) {
            ui_show_battery(battery.percent);
            gpio_set_level(PIN_LCD_BL, 1);
            vTaskDelay(pdMS_TO_TICKS(WAKE_BATTERY_MS));
            gpio_set_level(PIN_LCD_BL, 0);
        }
    }
    ui_show_home();
    gpio_set_level(PIN_LCD_BL, 1);
    s_touch_was_down = false;
    s_screen_on = true;
    s_screen_sleep_started_ms = 0;
    s_next_auto_poweroff_attempt_ms = 0;
    s_last_activity_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "screen wake");
}

static void handle_pkey_events(int64_t now_ms)
{
    if (now_ms < s_next_pkey_poll_ms) return;
    s_next_pkey_poll_ms = now_ms + PKEY_FAST_POLL_MS;

    uint8_t events = axp_take_pkey_events();
    if (!events) return;

    if (events & AXP2101_PKEY_LONG) {
        s_ignore_pkey_short_until_ms = now_ms + 1200;
        if (s_screen_on) {
            ui_show_power_message("POWERING OFF", C_AMBER);
        }
        audio_suspend();
        vTaskDelay(pdMS_TO_TICKS(650));
        ESP_LOGI(TAG, "PWR long press: requesting AXP2101 shutdown");
        if (!axp_shutdown()) {
            ESP_LOGE(TAG, "AXP2101 shutdown request failed; using screen-off fallback");
            screen_sleep();
        } else {
            /* The PMIC normally removes the processor rails immediately. If
             * VBUS policy keeps them up, leave a recoverable low-power screen-off
             * state instead of stranding the shutdown message. */
            vTaskDelay(pdMS_TO_TICKS(500));
            screen_sleep();
        }
        return;
    }

    if ((events & AXP2101_PKEY_SHORT) && now_ms >= s_ignore_pkey_short_until_ms &&
        !s_recording_active && !s_upload_active) {
        if (s_screen_on) screen_sleep();
        else screen_wake();
    }
}

/* ---------------- Touch (CST816D/CST816S-compatible controller) ---------------- */
static bool hit_rect(uint16_t x, uint16_t y, int x0, int y0, int x1, int y1)
{
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

static bool hit_disc(uint16_t x, uint16_t y, int cx, int cy, int r)
{
    int dx = (int)x - cx;
    int dy = (int)y - cy;
    return (dx * dx + dy * dy) <= (r * r);
}

static void touch_deinit(void)
{
    if (s_touch) {
        esp_lcd_touch_del(s_touch);
        s_touch = NULL;
    }
    if (s_touch_io) {
        esp_lcd_panel_io_del(s_touch_io);
        s_touch_io = NULL;
    }
    s_touch_was_down = false;
}

static esp_err_t touch_init(void)
{
    ESP_RETURN_ON_FALSE(s_i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "i2c bus not ready for touch");

    esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = PIN_TOUCH_INT,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_i2c_config_t touch_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &touch_io_cfg, &s_touch_io),
                        TAG, "touch i2c io");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(s_touch_io, &touch_cfg, &s_touch),
                        TAG, "cst816 touch");
    ESP_LOGI(TAG, "CST816 touch ready on SDA=%d SCL=%d INT=%d", PIN_I2C_SDA, PIN_I2C_SCL, PIN_TOUCH_INT);
    return ESP_OK;
}

static bool touch_press_edge(uint16_t *x, uint16_t *y)
{
    if (!s_touch) return false;

    esp_err_t err = esp_lcd_touch_read_data(s_touch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_touch_point_data_t point = {0};
    uint8_t points = 0;
    err = esp_lcd_touch_get_data(s_touch, &point, &points, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch data failed: %s", esp_err_to_name(err));
        return false;
    }
    if (points == 0) {
        s_touch_was_down = false;
        return false;
    }
    if (s_touch_was_down) return false;

    s_touch_was_down = true;
    *x = point.x;
    *y = point.y;
    ESP_LOGI(TAG, "touch tap x=%u y=%u", point.x, point.y);
    return true;
}

static bool touch_record_pressed(void)
{
    uint16_t x = 0, y = 0;
    if (!touch_press_edge(&x, &y)) return false;
    if (s_recording_active) return true;
    return hit_disc(x, y, 120, 180, 82);
}

static touch_action_t touch_home_action(void)
{
    uint16_t x = 0, y = 0;
    if (!touch_press_edge(&x, &y)) return TOUCH_ACTION_NONE;
    if (hit_rect(x, y, 20, 24, 220, 78)) {
        draw_clips_pill_pressed(pending_clip_count());
        return TOUCH_ACTION_CLIPS;
    }
    if (hit_disc(x, y, 120, 180, 82)) {
        ui_home_record_pressed();
        return TOUCH_ACTION_RECORD;
    }
    return TOUCH_ACTION_NONE;
}

static esp_err_t lcd_panel_reinitialize(void)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");

    /* Reapply tuning after every reset. An interrupted power handoff may leave
     * the ST7789 alive but back at its power-on register defaults. */
    static const struct { uint8_t cmd; uint8_t len; uint8_t data[14]; } st7789_tune[] = {
        {0xB2, 5, {0x0C, 0x0C, 0x00, 0x33, 0x33}},
        {0xB7, 1, {0x35}},
        {0xBB, 1, {0x19}},
        {0xC0, 1, {0x2C}},
        {0xC2, 1, {0x01}},
        {0xC3, 1, {0x12}},
        {0xC4, 1, {0x20}},
        {0xC6, 1, {0x0F}},
        {0xD0, 2, {0xA4, 0xA1}},
        {0xE0, 14, {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
                    0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23}},
        {0xE1, 14, {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
                    0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23}},
    };
    for (size_t i = 0; i < sizeof(st7789_tune) / sizeof(st7789_tune[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_panel_io, st7789_tune[i].cmd,
                                                       st7789_tune[i].data, st7789_tune[i].len),
                            TAG, "panel tuning");
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "panel invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel display on");
    s_panel_needs_reinit = false;
    return ESP_OK;
}

static esp_err_t lcd_init(void)
{
    gpio_config_t bl = { .pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(PIN_LCD_BL, 0);

    s_lcd_tx_done = xSemaphoreCreateCounting(LCD_V_RES + 32, 0);
    ESP_RETURN_ON_FALSE(s_lcd_tx_done != NULL, ESP_ERR_NO_MEM, TAG, "LCD completion semaphore");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_panel_io),
                        TAG, "panel io");

    esp_lcd_panel_io_callbacks_t io_callbacks = {
        .on_color_trans_done = lcd_color_transfer_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(s_panel_io, &io_callbacks, NULL),
                        TAG, "panel callbacks");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &panel_cfg, &s_panel), TAG, "st7789");
    return lcd_panel_reinitialize();
}

/* ---------------- SD ---------------- */
static bool sd_mount(void)
{
    if (s_sd_card) return true;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = LCD_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        /* Never format automatically in production. A transient contact or
         * brownout must not erase recordings that have not reached Hoth. */
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mcfg, &s_sd_card);
    if (err != ESP_OK) {
        s_sd_card = NULL;
        ESP_LOGE(TAG, "SD mount failed: 0x%x (%s)", err, esp_err_to_name(err));
        return false;
    }
    sdmmc_card_print_info(stdout, s_sd_card);
    return true;
}

static void sd_unmount(void)
{
    if (!s_sd_card) return;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_sd_card);
    if (err != ESP_OK) ESP_LOGW(TAG, "SD unmount failed: %s", esp_err_to_name(err));
    s_sd_card = NULL;
    s_sd_ok = false;
}

/* ---------------- Audio (ES7210) ---------------- */
static void audio_input_deinit(void)
{
    if (s_codec) {
        esp_codec_dev_close(s_codec);
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
    }
    if (s_rec_codec_if) {
        audio_codec_delete_codec_if(s_rec_codec_if);
        s_rec_codec_if = NULL;
    }
    if (s_rec_ctrl_if) {
        audio_codec_delete_ctrl_if(s_rec_ctrl_if);
        s_rec_ctrl_if = NULL;
    }
    if (s_rec_data_if) {
        audio_codec_delete_data_if(s_rec_data_if);
        s_rec_data_if = NULL;
    }
    if (s_i2s_rx) {
        i2s_channel_disable(s_i2s_rx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
    }
    s_audio_open = false;
}

static esp_err_t audio_resume(void)
{
    if (s_audio_open) return ESP_OK;
    ESP_RETURN_ON_FALSE(s_codec && s_i2s_rx, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = CHAN_NUM,
        .channel_mask = MIC_SELECTED,
        .sample_rate = SAMPLE_RATE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &fs) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "codec open");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec, MIC_GAIN_DB) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set gain");
    s_audio_open = true;
    vTaskDelay(pdMS_TO_TICKS(35));
    ESP_LOGI(TAG, "audio capture resumed");
    return ESP_OK;
}

static void audio_suspend(void)
{
    if (!s_audio_open || !s_codec) return;
    if (esp_codec_dev_close(s_codec) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "audio capture suspend failed");
        return;
    }
    s_audio_open = false;
    ESP_LOGI(TAG, "audio capture suspended");
}

static esp_err_t audio_init(void)
{
    if (s_codec && s_i2s_rx) return audio_resume();

    /* I2S RX channel in TDM mode (es7210 driver defaults to Philips format) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;      /* more/larger DMA buffers (~0.5s slack) so SD/LCD */
    chan_cfg.dma_frame_num = 1023;  /* stalls don't overflow the ring and click the audio */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx), TAG, "i2s new");

    i2s_tdm_config_t tdm_cfg = {
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(SAMPLE_BITS, I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1),
        .clk_cfg = {
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .sample_rate_hz = SAMPLE_RATE,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_WS,
            .dout = -1,           /* ES7210 is ADC-only */
            .din  = PIN_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_i2s_rx, &tdm_cfg), TAG, "i2s tdm");

    ESP_RETURN_ON_ERROR(shared_i2c_init(), TAG, "i2c bus");

    audio_codec_i2c_cfg_t ctrl_cfg = {
        .port = I2C_PORT,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    s_rec_ctrl_if = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    ESP_RETURN_ON_FALSE(s_rec_ctrl_if, ESP_FAIL, TAG, "es7210 i2c ctrl");

    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = 0,
        .rx_handle = s_i2s_rx,
        .tx_handle = NULL,
    };
    s_rec_data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    ESP_RETURN_ON_FALSE(s_rec_data_if, ESP_FAIL, TAG, "es7210 i2s data");

    es7210_codec_cfg_t es_cfg = {
        .ctrl_if = s_rec_ctrl_if,
        .master_mode = false,
        .mic_selected = MIC_SELECTED,
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = I2S_MCLK_MULTIPLE_256,
    };
    s_rec_codec_if = es7210_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(s_rec_codec_if, ESP_FAIL, TAG, "es7210 new");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_rec_codec_if,
        .data_if = s_rec_data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "codec dev new");

    ESP_RETURN_ON_ERROR(audio_resume(), TAG, "audio resume");
    ESP_LOGI(TAG, "ES7210 initialized: %d Hz, %d ch, 16-bit, gain %.0f dB",
             SAMPLE_RATE, CHAN_NUM, MIC_GAIN_DB);
    return ESP_OK;
}

static esp_err_t audio_init_with_retry(void)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        err = audio_init();
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "audio init attempt %d failed: %s", attempt, esp_err_to_name(err));
        audio_input_deinit();
        axp_init_battery();
        vTaskDelay(pdMS_TO_TICKS(80 * attempt));
    }
    return err;
}

static bool parse_clip_index(const char *name, int *index)
{
    if (strlen(name) != 11) return false;
    if (strncasecmp(name, "rec_", 4) != 0 || strcasecmp(name + 7, ".wav") != 0) {
        return false;
    }
    if (name[4] < '0' || name[4] > '9' ||
        name[5] < '0' || name[5] > '9' ||
        name[6] < '0' || name[6] > '9') {
        return false;
    }
    int value = (name[4] - '0') * 100 + (name[5] - '0') * 10 + (name[6] - '0');
    if (value <= 0 || value >= 1000) return false;
    *index = value;
    return true;
}

static int scan_clip_bitmap(uint8_t *bitmap)
{
    memset(bitmap, 0, CLIP_BITMAP_BYTES);
    DIR *d = opendir(CLIPS_DIR);
    if (!d) return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int idx = 0;
        if (parse_clip_index(e->d_name, &idx)) {
            bitmap[idx >> 3] |= (uint8_t)(1U << (idx & 7));
            count++;
        }
    }
    closedir(d);
    return count;
}

static bool clip_bitmap_has(const uint8_t *bitmap, int index)
{
    return (bitmap[index >> 3] & (uint8_t)(1U << (index & 7))) != 0;
}

/* pick the next unused /sdcard/clips/rec_NNN.wav */
static int next_index(void)
{
    static uint8_t used[CLIP_BITMAP_BYTES];
    scan_clip_bitmap(used);
    for (int i = 1; i < 1000; i++) {
        if (!clip_bitmap_has(used, i)) return i;
    }
    return -1;
}

static int pending_clip_count(void)
{
    static uint8_t used[CLIP_BITMAP_BYTES];
    return scan_clip_bitmap(used);
}

static int collect_clip_indices_page(int *indices, int max_indices, int offset)
{
    static uint8_t used[CLIP_BITMAP_BYTES];
    scan_clip_bitmap(used);
    int count = 0;
    int seen = 0;
    for (int i = 1; i < 1000 && count < max_indices; i++) {
        if (!clip_bitmap_has(used, i)) continue;
        if (seen++ < offset) continue;
        indices[count++] = i;
    }
    return count;
}

static void commit_pending_trash(void)
{
    if (!s_trash_pending) return;
    if (remove(s_trash_path) != 0) ESP_LOGW(TAG, "trash cleanup failed: %s", s_trash_path);
    s_trash_pending = false;
    s_trash_path[0] = '\0';
    s_trash_restore_path[0] = '\0';
}

static void auto_power_off_if_due(int64_t now_ms)
{
    if (s_screen_on || s_recording_active || s_upload_active ||
        s_screen_sleep_started_ms == 0 ||
        now_ms - s_screen_sleep_started_ms < AUTO_POWER_OFF_MS ||
        now_ms < s_next_auto_poweroff_attempt_ms) {
        return;
    }

    battery_info_t power = battery_read();
    if (!power.valid || power.vbus) {
        s_next_auto_poweroff_attempt_ms = now_ms + AUTO_POWER_RETRY_MS;
        return;
    }

    s_next_auto_poweroff_attempt_ms = now_ms + AUTO_POWER_RETRY_MS;
    commit_pending_trash();
    audio_suspend();
    ESP_LOGI(TAG, "10-minute battery standby elapsed; requesting AXP2101 shutdown");
    if (!axp_shutdown()) {
        ESP_LOGE(TAG, "automatic AXP2101 shutdown request failed; retrying in 60 seconds");
        return;
    }

    /* A successful battery-powered request normally removes VSYS immediately.
     * If execution continues, stay safely blank and retry later rather than
     * spinning or exposing a half-powered display. */
    vTaskDelay(pdMS_TO_TICKS(500));
    screen_sleep();
    ESP_LOGW(TAG, "AXP2101 shutdown request returned without removing power");
}

static uint32_t screen_off_poll_ms(int64_t now_ms)
{
    if (s_screen_sleep_started_ms == 0 ||
        now_ms - s_screen_sleep_started_ms < PKEY_FAST_WINDOW_MS) {
        return PKEY_FAST_POLL_MS;
    }
    return PKEY_SLOW_POLL_MS;
}

static bool undo_pending_trash(void)
{
    if (!s_trash_pending) return false;
    bool restored = rename(s_trash_path, s_trash_restore_path) == 0;
    if (!restored) ESP_LOGW(TAG, "trash restore failed: %s", s_trash_path);
    s_trash_pending = false;
    s_trash_path[0] = '\0';
    s_trash_restore_path[0] = '\0';
    return restored;
}

static bool delete_clip_index(int index)
{
    commit_pending_trash();
    snprintf(s_trash_restore_path, sizeof(s_trash_restore_path), CLIPS_DIR "/rec_%03d.wav", index);
    snprintf(s_trash_path, sizeof(s_trash_path), CLIPS_DIR "/rec_%03d.trash", index);
    if (rename(s_trash_restore_path, s_trash_path) == 0) {
        s_trash_pending = true;
        s_trash_deadline_ms = esp_timer_get_time() / 1000 + 2000;
        ESP_LOGI(TAG, "soft-deleted %s", s_trash_restore_path);
        return true;
    }
    ESP_LOGW(TAG, "delete failed: %s", s_trash_restore_path);
    s_trash_restore_path[0] = '\0';
    s_trash_path[0] = '\0';
    return false;
}

/* Ensure /sdcard/clips exists, and move any legacy rec_*.wav out of the card root into it. */
static void ensure_clips_folder(void)
{
    mkdir(CLIPS_DIR, 0777);

    DIR *d = opendir(MOUNT_POINT);
    if (!d) return;
    static char names[64][32];
    int count = 0;
    struct dirent *e;
    while (count < 64 && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strncmp(e->d_name, "rec_", 4) == 0 &&
            strcasecmp(e->d_name + n - 4, ".wav") == 0) {
            strlcpy(names[count++], e->d_name, sizeof(names[0]));
        }
    }
    closedir(d);

    int moved = 0;
    for (int i = 0; i < count; i++) {
        char src[96], dst[96];
        snprintf(src, sizeof(src), MOUNT_POINT "/%s", names[i]);
        snprintf(dst, sizeof(dst), CLIPS_DIR "/%s", names[i]);
        if (rename(src, dst) == 0) moved++;
    }
    if (moved) ESP_LOGI(TAG, "moved %d clip(s) from root into %s", moved, CLIPS_DIR);
}

static bool wav_checkpoint(FILE *f, uint32_t data_bytes, bool durable)
{
    wav_header_t hdr = WAV_HEADER_PCM_DEFAULT(data_bytes, 16, SAMPLE_RATE, CHAN_NUM);
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;
    if (fflush(f) != 0) return false;
    if (durable && fsync(fileno(f)) != 0) return false;
    return fseek(f, 0, SEEK_END) == 0;
}

static bool recording_space_available(uint32_t required_bytes)
{
    uint64_t total_bytes = 0, free_bytes = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total_bytes, &free_bytes) != ESP_OK) {
        ESP_LOGW(TAG, "could not read SD free space");
        return true;
    }
    return free_bytes >= (uint64_t)required_bytes + RECORDING_RESERVE_BYTES;
}

static bool parse_partial_index(const char *name, int *index)
{
    if (strlen(name) != 12 || strncasecmp(name, "rec_", 4) != 0 ||
        strcasecmp(name + 7, ".part") != 0) return false;
    if (name[4] < '0' || name[4] > '9' || name[5] < '0' || name[5] > '9' ||
        name[6] < '0' || name[6] > '9') return false;
    *index = (name[4] - '0') * 100 + (name[5] - '0') * 10 + (name[6] - '0');
    return *index > 0 && *index < 1000;
}

static void recover_partial_recordings(void)
{
    static char names[32][16];
    DIR *d = opendir(CLIPS_DIR);
    if (!d) return;
    int count = 0;
    struct dirent *e;
    while (count < 32 && (e = readdir(d)) != NULL) {
        int index = 0;
        if (parse_partial_index(e->d_name, &index)) {
            strlcpy(names[count++], e->d_name, sizeof(names[0]));
        }
    }
    closedir(d);

    for (int i = 0; i < count; i++) {
        int index = 0;
        if (!parse_partial_index(names[i], &index)) continue;
        char partial[96], final[96];
        snprintf(partial, sizeof(partial), CLIPS_DIR "/%s", names[i]);
        snprintf(final, sizeof(final), CLIPS_DIR "/rec_%03d.wav", index);
        FILE *f = fopen(partial, "rb+");
        if (!f) continue;
        bool ok = fseek(f, 0, SEEK_END) == 0;
        long size = ok ? ftell(f) : -1;
        uint32_t data_bytes = size > (long)sizeof(wav_header_t)
                            ? (uint32_t)(size - sizeof(wav_header_t)) : 0;
        ok = data_bytes > 0 && wav_checkpoint(f, data_bytes, true);
        fclose(f);
        if (ok && rename(partial, final) == 0) {
            ESP_LOGW(TAG, "recovered interrupted recording %s", final);
        } else if (data_bytes == 0) {
            remove(partial);
        } else {
            ESP_LOGE(TAG, "could not recover %s; retaining partial file", partial);
        }
    }
}

static bool device_ready(void)
{
    return s_sd_ok && s_audio_ok && s_touch_ok;
}

static void monitor_sd_health(int64_t now_ms)
{
    if (!s_sd_ok || now_ms < s_next_sd_health_ms || s_recording_active || s_upload_active) return;
    s_next_sd_health_ms = now_ms + 5000;
    struct stat st = {0};
    if (stat(CLIPS_DIR, &st) != 0) {
        ESP_LOGW(TAG, "SD card became unavailable");
        sd_unmount();
        if (s_screen_on) ui_show_error("NO SD");
    }
}

static void retry_unavailable_peripherals(int64_t now_ms)
{
    if (device_ready() || now_ms < s_next_peripheral_retry_ms ||
        s_recording_active || s_upload_active) return;
    s_next_peripheral_retry_ms = now_ms + 5000;

    if (!s_sd_ok) {
        s_sd_ok = sd_mount();
        if (s_sd_ok) {
            ensure_clips_folder();
            recover_partial_recordings();
        }
    }
    if (!s_audio_ok) {
        s_audio_ok = audio_init_with_retry() == ESP_OK;
        if (s_audio_ok) audio_suspend();
    }
    if (!s_touch_ok) {
        touch_deinit();
        s_touch_ok = touch_init() == ESP_OK;
    }
    if (device_ready() && s_screen_on) ui_show_home();
}

static uint8_t audio_level_from_pcm(const uint8_t *buf, size_t bytes)
{
    const int16_t *samples = (const int16_t *)buf;
    size_t count = bytes / sizeof(int16_t);
    if (count == 0) return 0;

    uint64_t acc = 0;
    size_t used = 0;
    for (size_t i = 0; i < count; i += 2) {  /* MIC1 sample from each stereo frame */
        int32_t v = samples[i];
        acc += (uint32_t)(v < 0 ? -v : v);
        used++;
    }
    if (used == 0) return 0;
    uint32_t avg = (uint32_t)(acc / used);

    int level = (int)((avg * 100) / 3200);
    if (level < 8 && avg > 120) level = 8;
    if (level > 100) level = 100;
    return (uint8_t)level;
}

/* Record audio to a WAV until stop is requested or the safety cap is reached. */
static bool record_wav(int index)
{
    if (index < 1) {
        ui_show_error("STORAGE FULL");
        vTaskDelay(pdMS_TO_TICKS(1200));
        return false;
    }

    const uint32_t byte_rate = SAMPLE_RATE * CHAN_NUM * 16 / 8;
    const uint32_t max_wav_size = byte_rate * MAX_RECORD_SECONDS;
    if (!recording_space_available(max_wav_size + sizeof(wav_header_t))) {
        ui_show_error("NO SPACE");
        vTaskDelay(pdMS_TO_TICKS(1200));
        return false;
    }

    if (audio_resume() != ESP_OK) {
        ESP_LOGE(TAG, "recording aborted: audio resume failed");
        ui_show_error("NO AUDIO");
        vTaskDelay(pdMS_TO_TICKS(900));
        return false;
    }

    char path[80], partial_path[80];
    snprintf(path, sizeof(path), CLIPS_DIR "/rec_%03d.wav", index);
    snprintf(partial_path, sizeof(partial_path), CLIPS_DIR "/rec_%03d.part", index);
    wav_header_t hdr = WAV_HEADER_PCM_DEFAULT(0, 16, SAMPLE_RATE, CHAN_NUM);

    FILE *f = fopen(partial_path, "wb+");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", partial_path);
        audio_suspend();
        sd_unmount();
        ui_show_error("SD ERROR");
        vTaskDelay(pdMS_TO_TICKS(1000));
        return false;
    }
    if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        ESP_LOGE(TAG, "WAV header write failed");
        fclose(f);
        audio_suspend();
        sd_unmount();
        ui_show_error("SD ERROR");
        vTaskDelay(pdMS_TO_TICKS(1000));
        return false;
    }

    ESP_LOGI(TAG, "Recording max %ds -> %s", MAX_RECORD_SECONDS, partial_path);
    s_active_clip_index = index;
    s_recording_active = true;
    ui_show_recording(0);

    /* drain stale DMA data so the clip starts clean (no pre-roll / startup click) */
    static uint8_t buf[4096];
    size_t drained = 0;
    while (i2s_channel_read(s_i2s_rx, buf, sizeof(buf), &drained, 0) == ESP_OK && drained > 0) { }

    uint32_t written = 0;
    int last_pct = -1;
    int last_ui_second = -1;
    int64_t last_level_ms = 0;
    int64_t start_tick = esp_timer_get_time() / 1000;
    uint32_t elapsed_ms = 0;
    uint32_t checkpoint_bytes = 0;
    bool write_ok = true;
    while (written < max_wav_size) {
        size_t to_read = sizeof(buf);
        if (max_wav_size - written < to_read) to_read = max_wav_size - written;
        size_t got = 0;
        esp_err_t r = i2s_channel_read(s_i2s_rx, buf, to_read, &got, pdMS_TO_TICKS(1000));
        if (r != ESP_OK) { ESP_LOGE(TAG, "i2s read: %s", esp_err_to_name(r)); break; }
        if (got == 0) continue;
        if (fwrite(buf, 1, got, f) != got) {
            ESP_LOGE(TAG, "SD write failed after %" PRIu32 " bytes", written);
            write_ok = false;
            break;
        }
        written += got;

        elapsed_ms = (uint32_t)((esp_timer_get_time() / 1000) - start_tick);
        int pct = (int)(100.0f * written / max_wav_size);
        if (pct / 20 != last_pct / 20) { ESP_LOGI(TAG, "  %d%%", pct); }
        last_pct = pct;
        int ui_second = elapsed_ms / 1000;
        if (ui_second != last_ui_second) {
            ui_recording_update_time(elapsed_ms);
            last_ui_second = ui_second;
        }
        if ((int64_t)elapsed_ms - last_level_ms >= 120) {
            ui_recording_update_levels(audio_level_from_pcm(buf, got));
            last_level_ms = elapsed_ms;
        }

        if (written - checkpoint_bytes >= byte_rate) {
            if (!wav_checkpoint(f, written, false)) {
                ESP_LOGE(TAG, "WAV checkpoint failed");
                write_ok = false;
                break;
            }
            checkpoint_bytes = written;
        }

        if (elapsed_ms > MIN_RECORD_MS && touch_record_pressed()) {
            ESP_LOGI(TAG, "Stop requested at %" PRIu32 "ms", elapsed_ms);
            break;
        }
    }
    s_recording_active = false;
    audio_suspend();
    ui_show_saving(elapsed_ms);
    write_ok = write_ok && written > 0 && wav_checkpoint(f, written, true);
    if (fclose(f) != 0) write_ok = false;
    if (write_ok && rename(partial_path, path) != 0) write_ok = false;
    if (!write_ok) {
        ESP_LOGE(TAG, "recording finalization failed; retained %s for recovery", partial_path);
        sd_unmount();
        ui_show_error("SD ERROR");
        vTaskDelay(pdMS_TO_TICKS(1200));
        ui_show_home_after_recording();
        return false;
    }
    ESP_LOGI(TAG, "Saved %s (%" PRIu32 " bytes)", path, written);
    ui_show_saved(elapsed_ms);
    ui_show_home_after_recording();
    return true;
}

/* ---------------- WiFi upload (creds + server URL from /sdcard/wifi.txt) ---------------- */
typedef struct {
    char ssid[64];
    char password[96];
    char server[128];
} wifi_conf_t;

/* Parse /sdcard/wifi.txt: one key=value per line (ssid=, password=, server=).
 * Blank lines and lines starting with '#' are ignored. */
static bool read_wifi_conf(wifi_conf_t *c)
{
    memset(c, 0, sizeof(*c));
    strlcpy(c->server, "http://10.0.0.240:8090", sizeof(c->server));  /* default */

    FILE *f = fopen(MOUNT_POINT "/wifi.txt", "r");
    if (!f) {
        ESP_LOGE(TAG, "wifi.txt not found on SD card");
        return false;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        for (size_t k = strlen(key); k > 0 && (key[k - 1] == ' ' || key[k - 1] == '\t'); ) key[--k] = '\0';
        for (size_t v = strlen(val); v > 0 && (val[v - 1] == '\n' || val[v - 1] == '\r' ||
                                               val[v - 1] == ' '  || val[v - 1] == '\t'); ) val[--v] = '\0';
        if (strcmp(key, "ssid") == 0)                                  strlcpy(c->ssid, val, sizeof(c->ssid));
        else if (strcmp(key, "password") == 0 || strcmp(key, "psk") == 0) strlcpy(c->password, val, sizeof(c->password));
        else if (strcmp(key, "server") == 0)                           strlcpy(c->server, val, sizeof(c->server));
    }
    fclose(f);
    if (!c->ssid[0]) {
        ESP_LOGE(TAG, "wifi.txt is missing 'ssid='");
        return false;
    }
    return true;
}

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_evt = NULL;
static bool s_netstack_ready = false;
static int s_wifi_retry = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "WiFi disconnected (reason=%d), retry %d", d ? d->reason : -1, s_wifi_retry);
        if (s_wifi_retry < 8) {
            s_wifi_retry++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_evt, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_wifi_retry = 0;
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_up(const wifi_conf_t *c)
{
    if (!s_netstack_ready) {
        esp_err_t e = nvs_flash_init();
        if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            e = nvs_flash_erase();
            if (e == ESP_OK) e = nvs_flash_init();
        }
        if (e != ESP_OK) return e;
        e = esp_netif_init();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
        e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
        if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        e = esp_wifi_init(&cfg);
        if (e != ESP_OK) return e;
        s_wifi_evt = xEventGroupCreate();
        if (!s_wifi_evt) return ESP_ERR_NO_MEM;
        e = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_handler, NULL, NULL);
        if (e != ESP_OK) return e;
        e = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                wifi_event_handler, NULL, NULL);
        if (e != ESP_OK) return e;
        s_netstack_ready = true;
    }

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, c->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, c->password, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = c->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) return err;

    s_wifi_retry = 0;
    xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    EventBits_t bits = 0;
    int stage = 0;
    for (int waited_ms = 0; waited_ms < 20000; waited_ms += 250) {
        bits = xEventGroupWaitBits(s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(250));
        if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
        if (bits & WIFI_FAIL_BIT) break;
        if ((waited_ms % 500) == 0) {
            stage = stage % 3 + 1;
            ui_set_state(UI_STATE_CONNECTING);
            draw_wifi_icon_stage(stage);
            keep_screen_awake();
        }
    }
    ESP_LOGE(TAG, "WiFi connect failed/timeout for ssid=%s", c->ssid);
    esp_wifi_stop();
    return ESP_FAIL;
}

static void wifi_down(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi stopped");
}

#define UP_BOUNDARY "----wordclipBoundary7MA4YWxkTrZu0gW"
#define MIN_UPLOAD_WAV_BYTES 45
#define HTTP_UPLOAD_INVALID -2

static bool wav_file_has_audio(const char *path, long *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size_out) *size_out = fsize;

    uint8_t header[44] = {0};
    size_t got = fread(header, 1, sizeof(header), f);
    fclose(f);

    if (fsize < MIN_UPLOAD_WAV_BYTES || got < sizeof(header)) return false;
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) return false;

    uint32_t data_size = (uint32_t)header[40] |
                         ((uint32_t)header[41] << 8) |
                         ((uint32_t)header[42] << 16) |
                         ((uint32_t)header[43] << 24);
    return data_size > 0;
}

static bool file_capture_timestamp(const char *path, char *out, size_t out_len)
{
    struct stat st = {0};
    if (stat(path, &st) != 0) return false;
    if (st.st_mtime < 1704067200) return false;  /* before 2024-01-01 means RTC was not set */

    struct tm tm_utc = {0};
    gmtime_r(&st.st_mtime, &tm_utc);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return true;
}

static void sync_time_from_response(const char *body)
{
    const char *key = strstr(body, "\"server_time\":\"");
    if (!key) return;
    key += strlen("\"server_time\":\"");

    int year, mon, day, hour, min, sec;
    if (sscanf(key, "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) != 6) return;
    if (year < 2024) return;

    struct tm tm_utc = {
        .tm_year = year - 1900,
        .tm_mon = mon - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min = min,
        .tm_sec = sec,
        .tm_isdst = 0,
    };
    time_t epoch = mktime(&tm_utc);
    if (epoch <= 0) return;

    struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
    settimeofday(&tv, NULL);
    if (!rtc_set_utc(epoch)) ESP_LOGW(TAG, "could not persist time to PCF85063");
    ESP_LOGI(TAG, "RTC synced from Hoth: %04d-%02d-%02dT%02d:%02d:%02d", year, mon, day, hour, min, sec);
}

static bool http_write_all(esp_http_client_handle_t client, const void *data, size_t length)
{
    const char *bytes = (const char *)data;
    size_t offset = 0;
    while (offset < length) {
        int written = esp_http_client_write(client, bytes + offset, length - offset);
        if (written <= 0) return false;
        offset += (size_t)written;
    }
    return true;
}

/* POST one WAV to <server>/api/upload as multipart/form-data (field "file").
 * Returns the HTTP status code, or -1 on a transport error. */
static int http_upload_file(const char *server, const char *fullpath, const char *filename)
{
    long checked_size = 0;
    if (!wav_file_has_audio(fullpath, &checked_size)) {
        ESP_LOGW(TAG, "skipping invalid/empty WAV %s (%ld bytes)", filename, checked_size);
        return HTTP_UPLOAD_INVALID;
    }

    FILE *f = fopen(fullpath, "rb");
    if (!f) { ESP_LOGE(TAG, "open %s failed", fullpath); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long fsize = ftell(f);
    if (fsize < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    char capture_part[192] = "";
    char captured_at[32] = "";
    if (file_capture_timestamp(fullpath, captured_at, sizeof(captured_at))) {
        snprintf(capture_part, sizeof(capture_part),
            "--" UP_BOUNDARY "\r\n"
            "Content-Disposition: form-data; name=\"capture_timestamp\"\r\n\r\n"
            "%s\r\n", captured_at);
    }

    char preamble[384];
    int plen = snprintf(preamble, sizeof(preamble),
        "%s"
        "--" UP_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", capture_part, filename);
    static const char epilogue[] = "\r\n--" UP_BOUNDARY "--\r\n";
    int elen = (int)(sizeof(epilogue) - 1);
    int content_length = plen + (int)fsize + elen;

    char url[192];
    snprintf(url, sizeof(url), "%s/api/upload", server);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fclose(f);
        ESP_LOGE(TAG, "http client allocation failed");
        return -1;
    }
    esp_http_client_set_header(client, "Content-Type",
                               "multipart/form-data; boundary=" UP_BOUNDARY);

    int status = -1;
    if (esp_http_client_open(client, content_length) != ESP_OK) {
        ESP_LOGE(TAG, "http open failed for %s", url);
        goto done;
    }
    if (!http_write_all(client, preamble, plen)) goto done;

    static uint8_t up_buf[2048];
    size_t n;
    while ((n = fread(up_buf, 1, sizeof(up_buf), f)) > 0) {
        if (!http_write_all(client, up_buf, n)) { ESP_LOGE(TAG, "body write failed"); goto done; }
    }
    if (ferror(f)) { ESP_LOGE(TAG, "SD read failed during upload"); goto done; }
    if (!http_write_all(client, epilogue, elen)) goto done;

    esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    char response_body[256] = {0};
    int body_len = esp_http_client_read_response(client, response_body, sizeof(response_body) - 1);
    if (body_len > 0) {
        response_body[body_len] = '\0';
        sync_time_from_response(response_body);
    }
    ESP_LOGI(TAG, "upload %s (%ld bytes) -> HTTP %d", filename, fsize, status);

done:
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

static void upload_progress(int done, int total)
{
    keep_screen_awake();
    int pct = (total > 0) ? (100 * done / total) : 100;
    ui_show_uploading(pct);
}

static void upload_pending_clips(void)
{
    /* WiFi and I2S are used in separate modes (per PRD): upload only happens while idle. */
    upload_busy_begin();
    ui_show_connecting();

    wifi_conf_t conf;
    if (!read_wifi_conf(&conf)) {
        ESP_LOGE(TAG, "Upload aborted: no usable /sdcard/wifi.txt");
        ui_show_error("NO WIFI");
        vTaskDelay(pdMS_TO_TICKS(1400));
        upload_busy_end();
        return;
    }
    ESP_LOGI(TAG, "Upload: connecting to ssid='%s' (server=%s)", conf.ssid, conf.server);
    if (wifi_up(&conf) != ESP_OK) {
        ui_show_error("NO WIFI");
        vTaskDelay(pdMS_TO_TICKS(1400));
        upload_busy_end();
        return;
    }

    /* Scan once so sparse numbering does not require 999 fopen calls twice. */
    static uint8_t pending[CLIP_BITMAP_BYTES];
    int total = scan_clip_bitmap(pending);
    ESP_LOGI(TAG, "Syncing %d clip(s) to %s", total, conf.server);
    upload_progress(0, total);

    /* Stage 3: upload every clip; delete each from the card only on a 2xx ACK. */
    int uploaded = 0, invalid = 0, failed = 0, processed = 0;
    for (int i = 1; i < 1000 && processed < total; i++) {
        if (!clip_bitmap_has(pending, i)) continue;
        char path[80], name[24];
        snprintf(path, sizeof(path), CLIPS_DIR "/rec_%03d.wav", i);
        snprintf(name, sizeof(name), "rec_%03d.wav", i);
        int status = http_upload_file(conf.server, path, name);
        if (status == HTTP_UPLOAD_INVALID) {
            invalid++;
            ESP_LOGW(TAG, "retaining invalid snap for user review: %s", name);
        } else if (status >= 200 && status < 300) {
            if (remove(path) == 0) uploaded++;
            else {
                failed++;
                ESP_LOGE(TAG, "server accepted %s but local delete failed", name);
            }
        } else {
            failed++;
            ESP_LOGE(TAG, "upload %s failed (HTTP %d), keeping on card", name, status);
        }
        processed++;
        upload_progress(processed, total);
    }

    wifi_down();
    ui_show_upload_result(uploaded, failed + invalid);
    vTaskDelay(pdMS_TO_TICKS(1500));
    upload_busy_end();
    ESP_LOGI(TAG, "Sync finished: %d uploaded, %d invalid, %d failed", uploaded, invalid, failed);
}

static void clip_menu_loop(void)
{
    int indices[3] = {0};
    int total = pending_clip_count();
    int offset = 0;
    int visible = collect_clip_indices_page(indices, 3, offset);
    ui_show_clip_menu(indices, visible, total, offset);

    while (1) {
        uint16_t x = 0, y = 0;
        if (touch_press_edge(&x, &y)) {
            s_last_activity_ms = esp_timer_get_time() / 1000;
            if (hit_rect(x, y, 20, 16, 220, 68)) {
                draw_upload_pill_pressed(pending_clip_count());
                commit_pending_trash();
                if (pending_clip_count() > 0) upload_pending_clips();
                return;
            }
            if (hit_rect(x, y, 76, 246, 164, 284)) {
                if (s_trash_pending) {
                    draw_back_button(true);
                    undo_pending_trash();
                    total = pending_clip_count();
                    visible = collect_clip_indices_page(indices, 3, offset);
                    ui_show_clip_menu(indices, visible, total, offset);
                    continue;
                }
                draw_back_button(true);
                return;
            }
            if (hit_rect(x, y, 20, 246, 70, 284)) {
                draw_icon_press_flash(42, 265, false);
                if (offset > 0) offset--;
                total = pending_clip_count();
                visible = collect_clip_indices_page(indices, 3, offset);
                ui_show_clip_menu(indices, visible, total, offset);
                continue;
            }
            if (hit_rect(x, y, 170, 246, 224, 284)) {
                draw_icon_press_flash(198, 265, false);
                if (offset + visible < total) offset++;
                total = pending_clip_count();
                visible = collect_clip_indices_page(indices, 3, offset);
                ui_show_clip_menu(indices, visible, total, offset);
                continue;
            }

            for (int i = 0; i < visible; i++) {
                int row_y = 70 + i * 64;
                if (hit_rect(x, y, 176, row_y, 240, row_y + 58)) {
                    draw_icon_press_flash(210, row_y + 29, true);
                    delete_clip_index(indices[i]);
                    total = pending_clip_count();
                    if (offset > 0 && offset + visible > total) offset--;
                    visible = collect_clip_indices_page(indices, 3, offset);
                    ui_show_clip_menu(indices, visible, total, offset);
                    break;
                }
            }
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        handle_pkey_events(now_ms);
        if (!s_screen_on) {
            commit_pending_trash();
            return;
        }
        if (s_trash_pending && now_ms >= s_trash_deadline_ms) {
            commit_pending_trash();
            ui_show_clip_menu(indices, visible, total, offset);
        }
        if (s_screen_on && !s_upload_active && (now_ms - s_last_activity_ms > SCREEN_IDLE_MS)) {
            commit_pending_trash();
            screen_sleep();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot reset reason=%d", esp_reset_reason());
    /* Restore PMIC-controlled rails before touching LCD, SD, audio, or touch.
     * A fully depleted battery can reset these registers to unsafe defaults. */
    esp_err_t i2c_err = shared_i2c_init();
    if (i2c_err == ESP_OK) {
        axp_init_battery();
        axp_enable_pkey_events();
        rtc_restore_system_time();
        battery_info_t initial_battery = battery_read();
        if (initial_battery.valid) {
            s_last_known_vbus = initial_battery.vbus;
            ESP_LOGI(TAG, "boot power: battery=%d%% voltage=%dmV vbus=%d charging=%d done=%d",
                     initial_battery.percent, initial_battery.millivolts, initial_battery.vbus,
                     initial_battery.charging, initial_battery.charge_done);
        }
    } else {
        ESP_LOGE(TAG, "shared I2C init failed: %s", esp_err_to_name(i2c_err));
    }

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_ERROR_CHECK(lcd_init());
    ui_show_power_message("STARTING", C_GREEN);
    gpio_set_level(PIN_LCD_BL, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    s_sd_ok = sd_mount();
    if (s_sd_ok) {
        ensure_clips_folder();
        recover_partial_recordings();
    }

    esp_err_t aerr = audio_init_with_retry();
    if (aerr != ESP_OK) {
        ESP_LOGE(TAG, "audio_init failed: %s -- check ES7210 / AXP rails", esp_err_to_name(aerr));
    }
    s_audio_ok = aerr == ESP_OK;
    esp_err_t terr = touch_init();
    if (terr != ESP_OK) {
        ESP_LOGE(TAG, "touch_init failed: %s -- onscreen controls unavailable", esp_err_to_name(terr));
    }
    s_touch_ok = terr == ESP_OK;
    if (s_audio_ok) audio_suspend();

    if (device_ready()) ui_show_home();
    else if (!s_sd_ok) ui_show_error("NO SD");
    else if (!s_touch_ok) ui_show_error("NO TOUCH");
    else ui_show_error("NO AUDIO");
    ESP_LOGI(TAG, "Ready=%d (sd=%d audio=%d touch=%d). PWR short toggles screen; hold PWR for shutdown; BOOT is debug-only.",
             device_ready(), s_sd_ok, s_audio_ok, s_touch_ok);

    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    esp_err_t pm_err = esp_pm_configure(&pm_config);
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "PM configure failed: %s", esp_err_to_name(pm_err));
    } else {
        ESP_LOGI(TAG, "PM enabled: 160/40 MHz, auto light sleep");
        esp_err_t lock_err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "upload", &s_upload_pm_lock);
        if (lock_err != ESP_OK) {
            ESP_LOGW(TAG, "upload PM lock create failed: %s", esp_err_to_name(lock_err));
        }
        lock_err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &s_usb_pm_lock);
        if (lock_err != ESP_OK) {
            ESP_LOGW(TAG, "USB PM lock create failed: %s", esp_err_to_name(lock_err));
        } else {
            usb_pm_poll(0, true);
        }
    }

    s_last_activity_ms = esp_timer_get_time() / 1000;
    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        usb_pm_poll(now_ms, false);
        handle_pkey_events(now_ms);
        if (!s_screen_on) {
            /* AXP PWR events are latched. Poll quickly for the first minute,
             * then once per second until true PMIC shutdown at ten minutes. */
            auto_power_off_if_due(now_ms);
            vTaskDelay(pdMS_TO_TICKS(screen_off_poll_ms(now_ms)));
            continue;
        }

        monitor_sd_health(now_ms);
        retry_unavailable_peripherals(now_ms);
        if (s_ui_state == UI_STATE_HOME) ui_home_poll_charge_indicator(now_ms);

        touch_action_t touch_action = s_ui_state == UI_STATE_HOME
                                    ? touch_home_action() : TOUCH_ACTION_NONE;
        if (touch_action != TOUCH_ACTION_NONE) {
            s_last_activity_ms = now_ms;
        }

        if (touch_action == TOUCH_ACTION_CLIPS) {
            clip_menu_loop();
            if (s_screen_on) ui_show_home();
            s_last_activity_ms = esp_timer_get_time() / 1000;
        }
        if (touch_action == TOUCH_ACTION_RECORD) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (device_ready()) {
                if (!record_wav(next_index()) && s_ui_state != UI_STATE_HOME) ui_show_home();
            } else if (!s_sd_ok) {
                ui_show_error("NO SD");
            } else if (!s_touch_ok) {
                ui_show_error("NO TOUCH");
            } else {
                ESP_LOGW(TAG, "not ready, ignoring onscreen record tap");
                ui_show_error("NO AUDIO");
            }
            s_last_activity_ms = esp_timer_get_time() / 1000;
        }

        /* Idle timeout: blank the LCD to prevent image retention and save power. */
        if (s_screen_on && !s_recording_active && !s_upload_active &&
            (now_ms - s_last_activity_ms > SCREEN_IDLE_MS)) {
            screen_sleep();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
