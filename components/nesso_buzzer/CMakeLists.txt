idf_component_register(
    SRCS         "src/nesso_buzzer.c"
    INCLUDE_DIRS "include"
    REQUIRES     nesso_bsp esp_driver_ledc esp_driver_gpio
    PRIV_REQUIRES esp_common freertos log
)
