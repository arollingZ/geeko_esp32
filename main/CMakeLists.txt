idf_component_register(
    SRCS "main.c"
         "tf_card.c"
         "wifi_mgr.c"
         "led_status.c"
         "lcd_st7789.c"
         "lcd_ui.c"
         "font8x16.c"
         "codec_bus.c"
         "es8388_diag.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c
             esp_driver_spi
             esp_driver_sdspi
             esp_driver_gpio
             esp_lcd
             sdmmc
             fatfs
             vfs
             esp_timer
             esp_common
             nvs_flash
             esp_wifi
             esp_event
             esp_netif
             esp_http_server
             esp_http_client
)
