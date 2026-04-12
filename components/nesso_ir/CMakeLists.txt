idf_component_register(
    SRCS         "src/nesso_ir.c"
    INCLUDE_DIRS "include"
    REQUIRES     nesso_bsp driver esp_driver_rmt
    PRIV_REQUIRES esp_common freertos log
)
