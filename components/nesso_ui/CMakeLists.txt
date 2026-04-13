idf_component_register(
    SRCS         "src/nesso_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nesso_bsp
        nesso_lcd
        nesso_buttons
        nesso_wardrive
        nesso_eapol
        nesso_wifi
        nesso_ir
        nesso_subghz
        nesso_buzzer
        nesso_ble
        nesso_portal
        nesso_zigbee
        nesso_sx1262
        esp_lcd
        esp_driver_i2c
    PRIV_REQUIRES
        esp_common
        freertos
        log
        esp_timer
)
