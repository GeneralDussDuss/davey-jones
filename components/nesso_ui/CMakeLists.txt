idf_component_register(
    SRCS         "src/nesso_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nesso_bsp
        nesso_lcd
        nesso_buttons
    PRIV_REQUIRES
        esp_common
        freertos
        log
        nesso_wardrive
        nesso_eapol
)
