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
        char path[64];
        snprintf(path, sizeof(path), MOUNT_POINT "/rec_%03d.wav", i);
        FILE *f = fopen(path, "r");
        if (!f) return i;
        fclose(f);
    }
    return 999;
}

static int pending_clip_count(void)
{
    int next = next_index();
    if (next <= 1) return 0;
    return next - 1;
}

/* Record audio to a WAV until stop is requested or the safety cap is reached. */
static void record_wav(int index)
{
    char path[64];
    snprintf(path, sizeof(path), MOUNT_POINT "/rec_%03d.wav", index);

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

static void upload_pending_clips(void)
{
    /* TODO: enable WiFi, POST each /sdcard/rec_NNN.wav to /api/upload, delete on ACK.
     * This is intentionally separate from capture mode; WiFi and I2S should not overlap. */
    ESP_LOGW(TAG, "Upload requested but WiFi upload transport is not wired yet");
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
    while (1) {
        if (++hb >= 50) {
            hb = 0;
            ESP_LOGI(TAG, "hb: sd=%d audio=%d btn=%d", s_sd_ok, aerr == ESP_OK,
                     gpio_get_level(PIN_BTN));
        }
        int level = gpio_get_level(PIN_BTN);
        touch_action_t touch_action = touch_home_action();
        if (touch_action == TOUCH_ACTION_UPLOAD) {
            upload_pending_clips();
            ui_show_home();
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
            }
        }
        prev = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
