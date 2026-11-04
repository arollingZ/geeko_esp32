idf_component_register(
    SRCS "main.c"
         "tf_card.c"
         "wifi_mgr.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_spi
             esp_driver_sdspi
             esp_driver_gpio
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
