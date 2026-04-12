idf_component_register(
    SRCS         "src/nesso_eapol.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nesso_wifi
        spiffs
    PRIV_REQUIRES
        esp_common
        freertos
        log
)
