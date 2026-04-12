idf_component_register(
    SRCS         "src/nesso_subghz.c"
    INCLUDE_DIRS "include"
    REQUIRES     nesso_bsp nesso_sx1262
    PRIV_REQUIRES esp_common freertos log esp_timer spiffs
)
