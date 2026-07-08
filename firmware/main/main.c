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
#include <time.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "freertos/event_groups.h"
#include "format_wav.h"

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

/* ---- RGB565 colors (byte-swapped at fill time for ST7789-over-SPI) ---- */
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_MAGENTA 0xF81F
#define C_CHARCOAL 0x2124
#define C_PANEL    0x18E3
#define C_MUTED    0xA534
#define C_DOT      0x39E7
#define C_DARK_RED 0xA145
#define C_BLACK    0x0000
#define C_BLUE     0x001F

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_touch_io = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static bool s_sd_ok = false;
static i2s_chan_handle_t s_i2s_rx = NULL;
static esp_codec_dev_handle_t s_codec = NULL;
static int s_active_clip_index = 0;
static bool s_recording_active = false;
static bool s_touch_was_down = false;
static bool s_screen_on = true;
static int64_t s_last_activity_ms = 0;

static inline uint16_t sw16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

static int next_index(void);
static int pending_clip_count(void);

typedef enum {
    TOUCH_ACTION_NONE = 0,
    TOUCH_ACTION_RECORD,
    TOUCH_ACTION_UPLOAD,
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

static void ui_show_home(void)
{
    lcd_fill(C_CHARCOAL);
    /* Upload pill. Text rendering moves to LVGL; this shape reserves the touch target. */
    lcd_rect(52, 32, 188, 78, C_PANEL);
    lcd_disc(52, 55, 23, C_PANEL);
    lcd_disc(188, 55, 23, C_PANEL);

    /* Large record target from the mockup. */
    lcd_disc(120, 178, 63, C_DARK_RED);
    lcd_disc(120, 178, 58, C_RED);

    ESP_LOGI(TAG, "UI home: pending=%d", pending_clip_count());
}

static void ui_show_recording(uint32_t elapsed_ms)
{
    static uint16_t row[LCD_H_RES];
    uint16_t bg = sw16(C_CHARCOAL), dot = sw16(C_DOT), muted = sw16(C_MUTED);
    for (int y = 0; y < LCD_V_RES; y++) {
        for (int x = 0; x < LCD_H_RES; x++) {
            bool is_dot = ((x % 16) == 0) && ((y % 16) == 0);
            row[x] = is_dot ? dot : bg;
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + 1, row);
    }

    /* REC dot and clip-index pill; timer text comes with the LVGL pass. */
    lcd_disc(38, 236, 7, C_RED);
    lcd_rect(182, 226, 226, 258, muted);
    lcd_disc(182, 242, 16, muted);
    lcd_disc(226, 242, 16, muted);

    int pct = (int)((elapsed_ms * 100) / (MAX_RECORD_SECONDS * 1000));
    ESP_LOGI(TAG, "UI recording: clip=%03d elapsed=%" PRIu32 "ms pct=%d", s_active_clip_index, elapsed_ms, pct);
}

static void ui_show_saved(void)
{
    lcd_fill(C_WHITE);
    vTaskDelay(pdMS_TO_TICKS(180));
    ui_show_home();
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
    ui_show_home();
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
    return hit_disc(x, y, 120, 178, 70);
}

static touch_action_t touch_home_action(void)
{
    uint16_t x = 0, y = 0;
    if (!touch_press_edge(&x, &y)) return TOUCH_ACTION_NONE;
    if (hit_rect(x, y, 29, 32, 211, 78)) return TOUCH_ACTION_UPLOAD;
    if (hit_disc(x, y, 120, 178, 70)) return TOUCH_ACTION_RECORD;
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
static esp_err_t audio_init(void)
{
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

    /* Shared I2C master bus for ES7210 control and CST816 touch. */
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG, "i2c bus");

    audio_codec_i2c_cfg_t ctrl_cfg = {
        .port = I2C_PORT,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "es7210 i2c ctrl");

    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = 0,
        .rx_handle = s_i2s_rx,
        .tx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "es7210 i2s data");

    es7210_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .master_mode = false,
        .mic_selected = MIC_SELECTED,
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = I2S_MCLK_MULTIPLE_256,
    };
    const audio_codec_if_t *es_if = es7210_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(es_if, ESP_FAIL, TAG, "es7210 new");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es_if,
        .data_if = data_if,
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

/* pick the next unused /sdcard/rec_NNN.wav */
static int next_index(void)
{
    for (int i = 1; i < 1000; i++) {
        char path[80];
        snprintf(path, sizeof(path), CLIPS_DIR "/rec_%03d.wav", i);
        FILE *f = fopen(path, "r");
        if (!f) return i;
        fclose(f);
    }
    return 999;
}

static int pending_clip_count(void)
{
    int total = 0;
    for (int i = 1; i < 1000; i++) {
        char path[80];
        snprintf(path, sizeof(path), CLIPS_DIR "/rec_%03d.wav", i);
        FILE *p = fopen(path, "r");
        if (p) { fclose(p); total++; }
    }
    return total;
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
    int64_t start_tick = esp_timer_get_time() / 1000;
    bool stop_armed = false;
    while (written < max_wav_size) {
        size_t to_read = sizeof(buf);
        if (max_wav_size - written < to_read) to_read = max_wav_size - written;
        size_t got = 0;
        esp_err_t r = i2s_channel_read(s_i2s_rx, buf, to_read, &got, pdMS_TO_TICKS(1000));
        if (r != ESP_OK) { ESP_LOGE(TAG, "i2s read: %s", esp_err_to_name(r)); break; }
        fwrite(buf, got, 1, f);
        written += got;

        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() / 1000) - start_tick);
        int pct = (int)(100.0f * written / max_wav_size);
        if (pct / 20 != last_pct / 20) { ESP_LOGI(TAG, "  %d%%", pct); }
        last_pct = pct;

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
    ui_show_saved();
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
    strlcpy(c->server, "http://10.0.0.240:8092", sizeof(c->server));  /* default */

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

/* Blue screen with a white progress bar filled to done/total. */
static void upload_progress(int done, int total)
{
    lcd_fill(C_BLUE);
    const int margin = 20;
    const int bar_w = LCD_H_RES - 2 * margin;
    const int bar_y = LCD_V_RES / 2 - 14;
    const int bar_h = 28;
    lcd_rect(margin, bar_y, margin + bar_w, bar_y + bar_h, C_CHARCOAL);   /* track */
    int fill_w = (total > 0) ? (bar_w * done / total) : 0;
    if (fill_w > 0) lcd_rect(margin, bar_y, margin + fill_w, bar_y + bar_h, C_WHITE);
}

static void upload_pending_clips(void)
{
    /* WiFi and I2S are used in separate modes (per PRD): upload only happens while idle. */
    lcd_fill(C_BLUE);   /* connecting… */

    wifi_conf_t conf;
    if (!read_wifi_conf(&conf)) {
        ESP_LOGE(TAG, "Upload aborted: no usable /sdcard/wifi.txt");
        lcd_fill(C_RED); vTaskDelay(pdMS_TO_TICKS(900)); return;
    }
    ESP_LOGI(TAG, "Upload: connecting to ssid='%s' (server=%s)", conf.ssid, conf.server);
    if (wifi_up(&conf) != ESP_OK) {
        lcd_fill(C_RED); vTaskDelay(pdMS_TO_TICKS(900)); return;
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
    lcd_fill(failed == 0 ? C_GREEN : C_RED);   /* green only on a fully clean sync */
    vTaskDelay(pdMS_TO_TICKS(1200));
    ESP_LOGI(TAG, "Sync finished: %d uploaded, %d skipped, %d failed", uploaded, skipped, failed);
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

        if (touch_action == TOUCH_ACTION_UPLOAD) {
            upload_pending_clips();
            ui_show_home();
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
