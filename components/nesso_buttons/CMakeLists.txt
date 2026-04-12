idf_component_register(
    SRCS         "src/nesso_buttons.c"
    INCLUDE_DIRS "include"
    REQUIRES     nesso_bsp pi4ioe5v6408
    PRIV_REQUIRES esp_common freertos log esp_timer
)
