idf_component_register(
    SRCS        "src/pi4ioe5v6408.c"
    INCLUDE_DIRS "include"
    REQUIRES    esp_driver_i2c
    PRIV_REQUIRES esp_common freertos log
)
