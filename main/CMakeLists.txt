idf_component_register(
    SRCS "main.c"
         "wifi_station.c"
         "radio_player.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_timer
        esp_driver_i2s
        esp_driver_gpio
        esp_driver_dac
        esp_driver_gptimer
        esp_driver_ledc
        esp_driver_uart
    PRIV_REQUIRES minimp3
)
