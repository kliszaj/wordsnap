/*
 * Wordclip capture-test firmware  --  Stage B
 * Board: Waveshare ESP32-C6-Touch-LCD-1.83
 *
 *   - LCD status display styled after the WordSnap UI mockups
 *   - Home view: upload pill + large red record button
 *   - Recording view: dark dotted field + stop-capable capture
 *   - microSD (FAT32) over shared SPI2 bus
 *   - Capacitive touch starts/stops recording; BOOT (GPIO9) remains a debug fallback
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
#include <stdlib.h>
#include <time.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/i2s_std.h"
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
#include "esp_check.h"
#include "esp_timer.h"
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
#define PIN_BTN     9          /* BOOT button, active-low */
#define PIN_TOUCH_INT 11
#define PIN_TOUCH_RST -1       /* Waveshare example leaves CST816 reset unmanaged; GPIO4 is LCD reset */
#define AXP2101_ADDR 0x34

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
#define PIN_I2S_DOUT    23

#define SAMPLE_RATE     16000                 /* Whisper-native */
#define SAMPLE_BITS     I2S_DATA_BIT_WIDTH_16BIT
#define CHAN_NUM        2                     /* MIC1 + MIC2 */
#define MIC_SELECTED    (ES7210_SEL_MIC1 | ES7210_SEL_MIC2)
#define MIC_GAIN_DB     37.5f          /* ES7210 analog PGA near max; raise level for quiet capture */
#define MAX_RECORD_SECONDS 15
#define MIN_RECORD_MS      600
#define SCREEN_IDLE_MS     30000       /* blank the LCD after this idle time (retention + power) */
#define WAKE_BATTERY_MS     2000

/* ---- RGB565 colors (byte-swapped at fill time for ST7789-over-SPI) ---- */
#define C_WHITE   0xFFFF
#define C_RED     0xFA69   /* #FF4D4D in RGB565 */
#define C_GREEN   0x07E0
#define C_AMBER   0xDD65
#define C_MAGENTA 0xF81F
#define C_BG       0x1082
#define C_PANEL    0x18E3
#define C_TEXT     0xBDBD
#define C_MUTED    0xA514
#define C_DOT      0x39E7
#define C_DARK_RED 0xA145
#define C_BLACK    0x0000
#define C_BLUE     0x001F
#define C_LINE     0x31A6
#define C_RING     0x0861
#define C_DISABLED 0x5AEB
#define C_CHARCOAL C_BG

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_touch_io = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_axp = NULL;
static bool s_sd_ok = false;
static i2s_chan_handle_t s_i2s_rx = NULL;
static i2s_chan_handle_t s_i2s_tx = NULL;
static esp_codec_dev_handle_t s_codec = NULL;
static esp_codec_dev_handle_t s_play_codec = NULL;
static const audio_codec_ctrl_if_t *s_rec_ctrl_if = NULL;
static const audio_codec_data_if_t *s_rec_data_if = NULL;
static const audio_codec_if_t *s_rec_codec_if = NULL;
static const audio_codec_ctrl_if_t *s_play_ctrl_if = NULL;
static const audio_codec_data_if_t *s_play_data_if = NULL;
static const audio_codec_gpio_if_t *s_play_gpio_if = NULL;
static const audio_codec_if_t *s_play_codec_if = NULL;
static int s_active_clip_index = 0;
static bool s_recording_active = false;
static bool s_touch_was_down = false;
static bool s_screen_on = true;
static int64_t s_last_activity_ms = 0;
static char s_timer_prev[6] = "";
static int s_wave_x = 0;
static int s_upload_prev_pct = -1;
static bool s_upload_prev_done = false;

static inline uint16_t sw16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

static int next_index(void);
static int pending_clip_count(void);
static bool parse_clip_index(const char *name, int *index);

typedef enum {
    TOUCH_ACTION_NONE = 0,
    TOUCH_ACTION_RECORD,
    TOUCH_ACTION_CLIPS,
} touch_action_t;

/* ---------------- LCD ---------------- */
static void lcd_fill(uint16_t color)
{
    static uint16_t line[LCD_H_RES];
    uint16_t v = sw16(color);
    for (int x = 0; x < LCD_H_RES; x++) line[x] = v;
    for (int y = 0; y < LCD_V_RES; y++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + 1, line);
    }
}

static void ui_transition_begin(void)
{
    if (s_screen_on) gpio_set_level(PIN_LCD_BL, 0);
}

static void ui_transition_end(void)
{
    if (s_screen_on) {
        gpio_set_level(PIN_LCD_BL, 1);
        vTaskDelay(pdMS_TO_TICKS(12));
    }
}

static void ui_transition_fill(uint16_t color)
{
    ui_transition_begin();
    lcd_fill(color);
}

static void lcd_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LCD_H_RES) x1 = LCD_H_RES;
    if (y1 > LCD_V_RES) y1 = LCD_V_RES;
    if (x1 <= x0 || y1 <= y0) return;

    static uint16_t row[LCD_H_RES];
    uint16_t v = sw16(color);
    int w = x1 - x0;
    for (int x = 0; x < w; x++) row[x] = v;
    for (int y = y0; y < y1; y++) {
        esp_lcd_panel_draw_bitmap(s_panel, x0, y, x1, y + 1, row);
    }
}

static void lcd_disc(int cx, int cy, int r, uint16_t color)
{
    static uint16_t row[LCD_H_RES];
    uint16_t v = sw16(color);
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
        if (w > 0) esp_lcd_panel_draw_bitmap(s_panel, x0, y, x1, y + 1, row);
    }
}

static void lcd_circle_outline(int cx, int cy, int r, int thickness, uint16_t color)
{
    lcd_disc(cx, cy, r, color);
    lcd_disc(cx, cy, r - thickness, C_BG);
}

static void lcd_round_rect_fill(int x, int y, int w, int h, int r, uint16_t color)
{
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
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        lcd_disc(x0, y0, thickness / 2, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
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
            for (int xx = 0; xx < g->w; xx++) {
                uint8_t byte = bitmap[g->offset + yy * row_bytes + xx / 8];
                if (byte & (1 << (7 - (xx % 8)))) {
                    lcd_rect(x + xx + g->xoff, y + yy + g->yoff,
                             x + xx + g->xoff + 1, y + yy + g->yoff + 1, color);
                }
            }
        }
        x += g->advance;
    }
}

static int label_width(const char *s)
{
    return font_text_width(ui_label_glyphs, UI_LABEL_GLYPH_COUNT, s);
}

static void draw_label_text(int x, int y, const char *s, uint16_t color)
{
    draw_font_text(ui_label_glyphs, UI_LABEL_GLYPH_COUNT, ui_label_bitmap, x, y, s, color);
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
    char buf[4];
    snprintf(buf, sizeof(buf), "%02d", clip_index % 100);
    lcd_round_rect_fill(174, 228, 44, 28, 14, C_TEXT);
    draw_label_text(190, 235, buf, C_BG);
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
    int w = label_width(label) + 36;
    int x = (LCD_H_RES - w) / 2;
    uint16_t color = enabled ? C_TEXT : C_DISABLED;
    lcd_round_rect_outline(x, y, w, 38, 19, 1, color, C_BG);
    draw_label_text(x + 18, y + 11, label, color);
}

static void draw_pill_pressed(int y, const char *label)
{
    int w = label_width(label) + 36;
    int x = (LCD_H_RES - w) / 2;
    lcd_round_rect_fill(x, y, w, 38, 19, C_TEXT);
    draw_label_text(x + 18, y + 11, label, C_BG);
    vTaskDelay(pdMS_TO_TICKS(45));
}

static void draw_clips_pill(int pending)
{
    char label[20];
    snprintf(label, sizeof(label), "CLIPS [%d]", pending);
    draw_pill_button(32, label, true);
}

static void draw_clips_pill_pressed(int pending)
{
    char label[20];
    snprintf(label, sizeof(label), "CLIPS [%d]", pending);
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
    int fill_w = pct <= 20 ? 16 : 38;
    uint16_t fill = pct <= 20 ? C_RED : C_AMBER;
    lcd_round_rect_fill(x + 5, y + 5, fill_w, h - 10, 11, fill);
    ui_transition_end();
}

static int battery_percent(void)
{
    if (!s_i2c_bus) return 50;
    if (!s_axp) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_ADDR,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(s_i2c_bus, &cfg, &s_axp) != ESP_OK) return 50;
    }
    uint8_t reg = 0xA4, pct = 0;
    if (i2c_master_transmit_receive(s_axp, &reg, 1, &pct, 1, 100) != ESP_OK) return 50;
    if (pct > 100) return 50;
    return pct;
}

static bool axp_vbus_good(void)
{
    if (!s_i2c_bus) return true;
    if (!s_axp) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_ADDR,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(s_i2c_bus, &cfg, &s_axp) != ESP_OK) return true;
    }
    uint8_t reg = 0x00, status = 0;
    if (i2c_master_transmit_receive(s_axp, &reg, 1, &status, 1, 100) != ESP_OK) return true;
    return (status & (1 << 5)) != 0;  /* AXP2101 reg00H[5] = VBUS_GOOD */
}

static void ui_show_home(void)
{
    ui_transition_fill(C_BG);
    draw_clips_pill(pending_clip_count());
    lcd_disc(120, 180, 67, C_RING);
    lcd_circle_outline(120, 180, 59, 3, C_PANEL);
    lcd_disc(120, 180, 47, C_RED);
    ui_transition_end();

    ESP_LOGI(TAG, "UI home: pending=%d", pending_clip_count());
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

static void draw_play_icon(int cx, int cy, uint16_t color)
{
    for (int row = -10; row <= 10; row++) {
        int half = 10 - abs(row);
        lcd_line(cx - 5, cy + row, cx - 5 + half, cy + row, 1, color);
    }
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

static void draw_back_button(void)
{
    const char *label = "BACK";
    int x = 78, y = 250, w = 84, h = 30;
    lcd_round_rect_outline(x, y, w, h, h / 2, 1, C_TEXT, C_BG);
    draw_label_text(x + (w - label_width(label)) / 2, y + 7, label, C_TEXT);
}

static void ui_show_clip_menu(const int *indices, int visible, int total, int offset)
{
    ui_transition_fill(C_BG);
    draw_upload_pill(total);

    if (total == 0) {
        draw_label_text((LCD_H_RES - label_width("NO CLIPS")) / 2, 132, "NO CLIPS", C_DISABLED);
    }

    for (int i = 0; i < visible; i++) {
        int y = 70 + i * 64;
        char label[16];
        snprintf(label, sizeof(label), "CLIP %03d", indices[i]);
        lcd_rect(18, y + 58, 222, y + 59, C_LINE);
        draw_play_icon(33, y + 29, C_TEXT);
        draw_label_text(58, y + 22, label, C_TEXT);
        draw_x_icon(210, y + 29, C_RED);
    }

    bool can_up = offset > 0;
    bool can_down = offset + visible < total;
    draw_icon_button(20, 250, 44, 30, can_up, false);
    draw_back_button();
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

static void ui_playback_update_levels(uint32_t frame)
{
    const int left = 0;
    const int right = LCD_H_RES;
    const int base = 198;
    const int top = 116;
    const int step = 6;
    const int bar_w = 4;
    int x = s_wave_x;
    if (x < left || x + bar_w >= right) x = left;

    lcd_rect(x, top, x + step + bar_w, base, C_BG);
    lcd_rect(x, base - 1, x + step + bar_w, base, C_LINE);

    int phase = (frame * 7 + x / step * 5) % 28;
    int folded = phase < 14 ? phase : 28 - phase;
    int h = 14 + folded * 4;
    if ((frame + x / step) % 5 == 0) h += 18;
    if (h > base - top) h = base - top;
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

static void ui_show_playback(int clip_index, uint32_t elapsed_ms)
{
    memset(s_timer_prev, 0, sizeof(s_timer_prev));
    s_wave_x = 0;
    ui_transition_fill(C_BG);
    lcd_rect(0, 198, LCD_H_RES, 200, C_LINE);
    draw_status(231, C_GREEN, "PLAY");
    draw_clip_pill(clip_index);
    ui_transition_end();
    ui_recording_update_time(elapsed_ms);
    lcd_rect(0, 116, LCD_H_RES, 198, C_BG);
    lcd_rect(0, 197, LCD_H_RES, 198, C_LINE);
}

static void ui_show_saved(uint32_t elapsed_ms)
{
    ui_recording_update_time(elapsed_ms);
    lcd_rect(0, 124, LCD_H_RES, 198, C_BG);
    lcd_rect(0, 198, LCD_H_RES, 200, C_LINE);
    draw_status(235, C_GREEN, "DONE");
    draw_clip_pill(s_active_clip_index);
    vTaskDelay(pdMS_TO_TICKS(900));
    ui_show_home();
}

static void ui_show_connecting(void)
{
    ui_transition_fill(C_BG);
    draw_label_text(92, 156, "...", C_TEXT);
    draw_status(230, C_AMBER, "CONNECTING...");
    ui_transition_end();
    s_upload_prev_pct = -1;
    s_upload_prev_done = false;
}

static void ui_show_uploading(int pct)
{
    bool done = pct >= 100;
    if (s_upload_prev_pct < 0) {
        ui_transition_fill(C_BG);
        ui_transition_end();
    }
    if (s_upload_prev_pct != pct) {
        lcd_rect(20, 96, 220, 170, C_BG);
    }
    char label[8];
    snprintf(label, sizeof(label), "%d%%", pct);
    draw_big_text((LCD_H_RES - big_width(label)) / 2, 104, label, C_TEXT);
    if (s_upload_prev_done != done || s_upload_prev_pct < 0) {
        draw_status(230, C_GREEN, done ? "DONE" : "UPLOADING...");
    }
    s_upload_prev_pct = pct;
    s_upload_prev_done = done;
}

static void ui_show_error(const char *label)
{
    ui_transition_fill(C_BG);
    draw_face();
    draw_status(230, C_RED, label);
    ui_transition_end();
}

/* ---------------- Screen power (idle blank to avoid LCD image retention) ---------------- */
static void screen_sleep(void)
{
    lcd_fill(C_BLACK);              /* rest the pixels (no static red held) */
    gpio_set_level(PIN_LCD_BL, 0);  /* backlight off saves power on the LiPo */
    s_screen_on = false;
    ESP_LOGI(TAG, "screen sleep (idle)");
}

static void screen_wake(void)
{
    gpio_set_level(PIN_LCD_BL, 1);
    if (!axp_vbus_good()) {
        ui_show_battery(battery_percent());
        vTaskDelay(pdMS_TO_TICKS(WAKE_BATTERY_MS));
    }
    ui_show_home();
    s_touch_was_down = false;
    s_screen_on = true;
    s_last_activity_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "screen wake");
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

static esp_err_t lcd_init(void)
{
    gpio_config_t bl = { .pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(PIN_LCD_BL, 1);

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io),
                        TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel), TAG, "st7789");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    return ESP_OK;
}

/* ---------------- SD ---------------- */
static bool sd_mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = LCD_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mcfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: 0x%x (%s)", err, esp_err_to_name(err));
        return false;
    }
    sdmmc_card_print_info(stdout, card);
    return true;
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
}

static void audio_output_deinit(void)
{
    if (s_play_codec) {
        esp_codec_dev_close(s_play_codec);
        esp_codec_dev_delete(s_play_codec);
        s_play_codec = NULL;
    }
    if (s_play_codec_if) {
        audio_codec_delete_codec_if(s_play_codec_if);
        s_play_codec_if = NULL;
    }
    if (s_play_ctrl_if) {
        audio_codec_delete_ctrl_if(s_play_ctrl_if);
        s_play_ctrl_if = NULL;
    }
    if (s_play_gpio_if) {
        audio_codec_delete_gpio_if(s_play_gpio_if);
        s_play_gpio_if = NULL;
    }
    if (s_play_data_if) {
        audio_codec_delete_data_if(s_play_data_if);
        s_play_data_if = NULL;
    }
    if (s_i2s_tx) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
    }
}

static esp_err_t audio_init(void)
{
    if (s_codec && s_i2s_rx) return ESP_OK;

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

    if (!s_i2c_bus) {
        /* Shared I2C master bus for ES7210, ES8311, CST816 touch, and AXP2101. */
        i2c_master_bus_config_t i2c_cfg = {
            .i2c_port = I2C_PORT,
            .sda_io_num = PIN_I2C_SDA,
            .scl_io_num = PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG, "i2c bus");
    }

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

    ESP_LOGI(TAG, "ES7210 ready: %d Hz, %d ch, 16-bit, gain %.0f dB",
             SAMPLE_RATE, CHAN_NUM, MIC_GAIN_DB);
    return ESP_OK;
}

static esp_err_t audio_output_init(uint32_t sample_rate, int channels, int bits_per_sample)
{
    if (s_play_codec && s_i2s_tx) return ESP_OK;
    if (channels != 1 && channels != 2) channels = 2;
    if (bits_per_sample != 16 && bits_per_sample != 32) bits_per_sample = 16;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL), TAG, "i2s tx new");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits_per_sample,
                                                        channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "i2s tx std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "i2s tx enable");

    if (!s_i2c_bus) {
        i2c_master_bus_config_t i2c_cfg = {
            .i2c_port = I2C_PORT,
            .sda_io_num = PIN_I2C_SDA,
            .scl_io_num = PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG, "i2c bus");
    }

    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = NULL,
        .tx_handle = s_i2s_tx,
    };
    s_play_data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    ESP_RETURN_ON_FALSE(s_play_data_if, ESP_FAIL, TAG, "es8311 i2s data");

    audio_codec_i2c_cfg_t ctrl_cfg = {
        .port = I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    s_play_ctrl_if = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    ESP_RETURN_ON_FALSE(s_play_ctrl_if, ESP_FAIL, TAG, "es8311 i2c ctrl");
    s_play_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_play_gpio_if, ESP_FAIL, TAG, "es8311 gpio");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = s_play_ctrl_if,
        .gpio_if = s_play_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .use_mclk = false,
    };
    s_play_codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(s_play_codec_if, ESP_FAIL, TAG, "es8311 new");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_play_codec_if,
        .data_if = s_play_data_if,
    };
    s_play_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_play_codec, ESP_FAIL, TAG, "play codec dev new");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits_per_sample,
        .channel = channels,
        .sample_rate = sample_rate,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_play_codec, &fs) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "play codec open");
    esp_codec_dev_set_out_vol(s_play_codec, 75);

    ESP_LOGI(TAG, "ES8311 ready: %" PRIu32 " Hz, %d ch, %d-bit", sample_rate, channels, bits_per_sample);
    return ESP_OK;
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

static int compare_ints(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

static int collect_clip_indices_all(int *indices, int max_indices)
{
    DIR *d = opendir(CLIPS_DIR);
    if (!d) return 0;
    int count = 0;
    struct dirent *e;
    while (count < max_indices && (e = readdir(d)) != NULL) {
        int idx = 0;
        if (parse_clip_index(e->d_name, &idx)) indices[count++] = idx;
    }
    closedir(d);
    qsort(indices, count, sizeof(indices[0]), compare_ints);
    return count;
}

/* pick the next unused /sdcard/clips/rec_NNN.wav */
static int next_index(void)
{
    bool used[1000] = {0};
    int indices[999];
    int count = collect_clip_indices_all(indices, 999);
    for (int i = 0; i < count; i++) used[indices[i]] = true;
    for (int i = 1; i < 1000; i++) {
        if (!used[i]) return i;
    }
    return 999;
}

static int pending_clip_count(void)
{
    int indices[999];
    return collect_clip_indices_all(indices, 999);
}

static int collect_clip_indices_page(int *indices, int max_indices, int offset)
{
    int all[999];
    int total = collect_clip_indices_all(all, 999);
    int count = 0;
    for (int i = offset; i < total && count < max_indices; i++) indices[count++] = all[i];
    return count;
}

static bool delete_clip_index(int index)
{
    char path[80];
    snprintf(path, sizeof(path), CLIPS_DIR "/rec_%03d.wav", index);
    if (remove(path) == 0) {
        ESP_LOGI(TAG, "deleted %s", path);
        return true;
    }
    ESP_LOGW(TAG, "delete failed: %s", path);
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
static void record_wav(int index)
{
    char path[80];
    snprintf(path, sizeof(path), CLIPS_DIR "/rec_%03d.wav", index);

    const uint32_t byte_rate = SAMPLE_RATE * CHAN_NUM * 16 / 8;
    const uint32_t max_wav_size  = byte_rate * MAX_RECORD_SECONDS;
    wav_header_t hdr = WAV_HEADER_PCM_DEFAULT(max_wav_size, 16, SAMPLE_RATE, CHAN_NUM);

    FILE *f = fopen(path, "w");
    if (!f) { ESP_LOGE(TAG, "open %s failed", path); return; }
    fwrite(&hdr, sizeof(hdr), 1, f);

    ESP_LOGI(TAG, "Recording max %ds -> %s", MAX_RECORD_SECONDS, path);
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
    bool stop_armed = false;
    while (written < max_wav_size) {
        size_t to_read = sizeof(buf);
        if (max_wav_size - written < to_read) to_read = max_wav_size - written;
        size_t got = 0;
        esp_err_t r = i2s_channel_read(s_i2s_rx, buf, to_read, &got, pdMS_TO_TICKS(1000));
        if (r != ESP_OK) { ESP_LOGE(TAG, "i2s read: %s", esp_err_to_name(r)); break; }
        fwrite(buf, got, 1, f);
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

        if (gpio_get_level(PIN_BTN) == 1) stop_armed = true;
        bool stop_pressed = stop_armed && gpio_get_level(PIN_BTN) == 0;
        if (elapsed_ms > MIN_RECORD_MS && (stop_pressed || touch_record_pressed())) {
            ESP_LOGI(TAG, "Stop requested at %" PRIu32 "ms", elapsed_ms);
            break;
        }
    }
    hdr = (wav_header_t)WAV_HEADER_PCM_DEFAULT(written, 16, SAMPLE_RATE, CHAN_NUM);
    fseek(f, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, f);
    fclose(f);
    s_recording_active = false;
    ESP_LOGI(TAG, "Saved %s (%" PRIu32 " bytes)", path, written);
    ui_show_saved(elapsed_ms);
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
            ESP_ERROR_CHECK(nvs_flash_erase());
            ESP_ERROR_CHECK(nvs_flash_init());
        }
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_evt = xEventGroupCreate();
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler, NULL, NULL));
        s_netstack_ready = true;
    }

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, c->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, c->password, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = c->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    s_wifi_retry = 0;
    xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
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
    ESP_LOGI(TAG, "RTC synced from Hoth: %04d-%02d-%02dT%02d:%02d:%02d", year, mon, day, hour, min, sec);
}

/* POST one WAV to <server>/api/upload as multipart/form-data (field "file").
 * Returns the HTTP status code, or -1 on a transport error. */
static int http_upload_file(const char *server, const char *fullpath, const char *filename)
{
    long checked_size = 0;
    if (!wav_file_has_audio(fullpath, &checked_size)) {
        ESP_LOGW(TAG, "skipping invalid/empty WAV %s (%ld bytes)", filename, checked_size);
        return 0;  /* Local junk cleanup; no server request was made. */
    }

    FILE *f = fopen(fullpath, "rb");
    if (!f) { ESP_LOGE(TAG, "open %s failed", fullpath); return -1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

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
    esp_http_client_set_header(client, "Content-Type",
                               "multipart/form-data; boundary=" UP_BOUNDARY);

    int status = -1;
    if (esp_http_client_open(client, content_length) != ESP_OK) {
        ESP_LOGE(TAG, "http open failed for %s", url);
        goto done;
    }
    if (esp_http_client_write(client, preamble, plen) < 0) goto done;

    static uint8_t up_buf[2048];
    size_t n;
    while ((n = fread(up_buf, 1, sizeof(up_buf), f)) > 0) {
        if (esp_http_client_write(client, (char *)up_buf, n) < 0) { ESP_LOGE(TAG, "body write failed"); goto done; }
    }
    if (esp_http_client_write(client, epilogue, elen) < 0) goto done;

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
    int pct = (total > 0) ? (100 * done / total) : 100;
    ui_show_uploading(pct);
}

static void upload_pending_clips(void)
{
    /* WiFi and I2S are used in separate modes (per PRD): upload only happens while idle. */
    ui_show_connecting();

    wifi_conf_t conf;
    if (!read_wifi_conf(&conf)) {
        ESP_LOGE(TAG, "Upload aborted: no usable /sdcard/wifi.txt");
        ui_show_error("NO WIFI"); vTaskDelay(pdMS_TO_TICKS(1400)); return;
    }
    ESP_LOGI(TAG, "Upload: connecting to ssid='%s' (server=%s)", conf.ssid, conf.server);
    if (wifi_up(&conf) != ESP_OK) {
        ui_show_error("NO WIFI"); vTaskDelay(pdMS_TO_TICKS(1400)); return;
    }

    /* Count clips to sync so we can show progress. */
    int total = 0;
    for (int i = 1; i < 1000; i++) {
        char path[80];
        snprintf(path, sizeof(path), CLIPS_DIR "/rec_%03d.wav", i);
        FILE *p = fopen(path, "r");
        if (p) { fclose(p); total++; }
    }
    ESP_LOGI(TAG, "Syncing %d clip(s) to %s", total, conf.server);
    upload_progress(0, total);

    /* Stage 3: upload every clip; delete each from the card only on a 2xx ACK. */
    int uploaded = 0, skipped = 0, failed = 0, processed = 0;
    for (int i = 1; i < 1000 && processed < total; i++) {
        char path[80], name[24];
        snprintf(path, sizeof(path), CLIPS_DIR "/rec_%03d.wav", i);
        snprintf(name, sizeof(name), "rec_%03d.wav", i);
        FILE *probe = fopen(path, "r");
        if (!probe) continue;
        fclose(probe);

        int status = http_upload_file(conf.server, path, name);
        if (status == 0) {
            remove(path);
            skipped++;
        } else if (status >= 200 && status < 300) {
            remove(path);
            uploaded++;
        } else {
            failed++;
            ESP_LOGE(TAG, "upload %s failed (HTTP %d), keeping on card", name, status);
        }
        processed++;
        upload_progress(processed, total);
    }

    wifi_down();
    if (failed == 0) ui_show_uploading(100);
    else ui_show_error("NO SERVER");
    vTaskDelay(pdMS_TO_TICKS(1200));
    ESP_LOGI(TAG, "Sync finished: %d uploaded, %d skipped, %d failed", uploaded, skipped, failed);
}

static void preview_clip_index(int index)
{
    char path[80];
    snprintf(path, sizeof(path), CLIPS_DIR "/rec_%03d.wav", index);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "preview open failed: %s", path);
        ui_show_error("NO CLIP");
        vTaskDelay(pdMS_TO_TICKS(900));
        return;
    }

    wav_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr.descriptor_chunk.chunk_id, "RIFF", 4) != 0 ||
        memcmp(hdr.descriptor_chunk.chunk_format, "WAVE", 4) != 0 ||
        memcmp(hdr.data_chunk.subchunk_id, "data", 4) != 0 ||
        hdr.fmt_chunk.audio_format != 1 ||
        (hdr.fmt_chunk.bits_per_sample != 16 && hdr.fmt_chunk.bits_per_sample != 32)) {
        fclose(f);
        ESP_LOGW(TAG, "preview unsupported WAV: %s", path);
        ui_show_error("NO CLIP");
        vTaskDelay(pdMS_TO_TICKS(900));
        return;
    }

    uint32_t sample_rate = hdr.fmt_chunk.sample_rate ? hdr.fmt_chunk.sample_rate : SAMPLE_RATE;
    int channels = hdr.fmt_chunk.num_of_channels ? hdr.fmt_chunk.num_of_channels : CHAN_NUM;
    int bits = hdr.fmt_chunk.bits_per_sample;

    ui_show_playback(index, 0);
    audio_input_deinit();
    if (audio_output_init(sample_rate, channels, bits) != ESP_OK) {
        fclose(f);
        audio_output_deinit();
        ESP_LOGE(TAG, "preview audio output init failed");
        ui_show_error("NO AUDIO");
        vTaskDelay(pdMS_TO_TICKS(900));
        audio_init();
        return;
    }

    uint8_t buf[2048];
    uint32_t remaining = hdr.data_chunk.subchunk_size;
    uint32_t played = 0;
    int64_t last_bar_ms = 0;
    uint32_t frame = 0;

    while (remaining > 0) {
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        size_t got = fread(buf, 1, want, f);
        if (got == 0) break;
        if (esp_codec_dev_write(s_play_codec, buf, (int)got) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "preview write failed");
            break;
        }
        played += got;
        remaining -= got;

        uint32_t elapsed_ms = (hdr.fmt_chunk.byte_rate > 0) ? (played * 1000U / hdr.fmt_chunk.byte_rate) : 0;
        ui_recording_update_time(elapsed_ms);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_bar_ms >= 70) {
            ui_playback_update_levels(frame++);
            last_bar_ms = now_ms;
        }
    }

    fclose(f);
    audio_output_deinit();
    audio_init();
    draw_status(235, C_GREEN, "DONE");
    vTaskDelay(pdMS_TO_TICKS(350));
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
                if (pending_clip_count() > 0) upload_pending_clips();
                return;
            }
            if (hit_rect(x, y, 76, 246, 164, 284)) {
                draw_pill_pressed(250, "BACK");
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
                if (hit_rect(x, y, 0, row_y, 70, row_y + 58)) {
                    draw_icon_press_flash(33, row_y + 29, false);
                    preview_clip_index(indices[i]);
                    total = pending_clip_count();
                    if (offset >= total && offset > 0) offset = total - 1;
                    visible = collect_clip_indices_page(indices, 3, offset);
                    ui_show_clip_menu(indices, visible, total, offset);
                    break;
                }
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
        if (s_screen_on && (now_ms - s_last_activity_ms > SCREEN_IDLE_MS)) {
            screen_sleep();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
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
    lcd_fill(C_CHARCOAL);
    vTaskDelay(pdMS_TO_TICKS(300));

    s_sd_ok = sd_mount();
    if (s_sd_ok) ensure_clips_folder();

    esp_err_t aerr = audio_init();
    if (aerr != ESP_OK) {
        ESP_LOGE(TAG, "audio_init failed: %s -- check ES7210 / AXP rails", esp_err_to_name(aerr));
    }
    esp_err_t terr = touch_init();
    if (terr != ESP_OK) {
        ESP_LOGE(TAG, "touch_init failed: %s -- BOOT fallback remains available", esp_err_to_name(terr));
    }
    bool ready = s_sd_ok && (aerr == ESP_OK);

    if (ready) ui_show_home();
    else lcd_fill(C_MAGENTA);
    ESP_LOGI(TAG, "Ready=%d (sd=%d audio=%d touch=%d). Tap record or press BOOT to record; tap/press again to stop.",
             ready, s_sd_ok, aerr == ESP_OK, terr == ESP_OK);

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    int prev = 1, hb = 0;
    s_last_activity_ms = esp_timer_get_time() / 1000;
    while (1) {
        if (++hb >= 50) {
            hb = 0;
            ESP_LOGI(TAG, "hb: sd=%d audio=%d btn=%d scr=%d", s_sd_ok, aerr == ESP_OK,
                     gpio_get_level(PIN_BTN), s_screen_on);
        }
        int64_t now_ms = esp_timer_get_time() / 1000;
        int level = gpio_get_level(PIN_BTN);

        /* Screen asleep: any touch or BOOT press only wakes it (does not act). */
        if (!s_screen_on) {
            uint16_t tx = 0, ty = 0;
            if (touch_press_edge(&tx, &ty) || (prev == 1 && level == 0)) {
                screen_wake();
            }
            prev = level;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        touch_action_t touch_action = touch_home_action();
        if (touch_action != TOUCH_ACTION_NONE || (prev == 1 && level == 0)) {
            s_last_activity_ms = now_ms;
        }

        if (touch_action == TOUCH_ACTION_CLIPS) {
            clip_menu_loop();
            if (s_screen_on) ui_show_home();
            s_last_activity_ms = esp_timer_get_time() / 1000;
        }
        if ((prev == 1 && level == 0) || touch_action == TOUCH_ACTION_RECORD) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (touch_action == TOUCH_ACTION_RECORD || gpio_get_level(PIN_BTN) == 0) {
                if (ready) {
                    record_wav(next_index());
                } else {
                    ESP_LOGW(TAG, "not ready, ignoring press");
                }
                if (ready) ui_show_home();
                else lcd_fill(C_MAGENTA);
                while (gpio_get_level(PIN_BTN) == 0) vTaskDelay(pdMS_TO_TICKS(20));
                s_last_activity_ms = esp_timer_get_time() / 1000;
            }
        }

        /* Idle timeout: blank the LCD to prevent image retention and save power. */
        if (s_screen_on && !s_recording_active &&
            (now_ms - s_last_activity_ms > SCREEN_IDLE_MS)) {
            screen_sleep();
        }

        prev = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
