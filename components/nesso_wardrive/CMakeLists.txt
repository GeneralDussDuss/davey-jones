idf_component_register(
    SRCS         "src/nesso_wardrive.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nesso_wifi
        spiffs
    PRIV_REQUIRES
        esp_common
        esp_wifi
        freertos
        log
        esp_timer
)
