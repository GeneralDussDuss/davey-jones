idf_component_register(
    SRCS         "src/nesso_bsp.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_driver_i2c pi4ioe5v6408
    PRIV_REQUIRES esp_common freertos log
)
