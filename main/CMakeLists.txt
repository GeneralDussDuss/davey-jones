idf_component_register(
    SRCS         "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nesso_bsp
        nesso_spi
        nesso_lcd
        nesso_buttons
        nesso_ui
        nesso_wifi
        nesso_wardrive
        nesso_eapol
        nesso_sx1262
)
