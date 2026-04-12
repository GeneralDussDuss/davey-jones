idf_component_register(
    SRCS         "src/nesso_lcd.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_lcd nesso_bsp nesso_spi
    PRIV_REQUIRES esp_common freertos log
)
