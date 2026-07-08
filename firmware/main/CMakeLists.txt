idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd fatfs sdmmc esp_driver_spi esp_driver_gpio esp_driver_i2s esp_driver_i2c esp_timer esp_lcd_touch esp_lcd_touch_cst816s esp_wifi esp_netif esp_event nvs_flash esp_http_client
)
