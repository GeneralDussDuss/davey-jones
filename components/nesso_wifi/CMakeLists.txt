idf_component_register(
    SRCS         "src/nesso_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
    PRIV_REQUIRES
        esp_common
        freertos
        log
        nvs_flash
)
