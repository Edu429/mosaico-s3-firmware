idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_netif esp_wifi nvs_flash esp_app_format app_update esp_partition spi_flash esp_timer esp_system esp_driver_i2c
)
