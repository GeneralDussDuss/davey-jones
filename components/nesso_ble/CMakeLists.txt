idf_component_register(
    SRCS         "src/nesso_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES     bt
    PRIV_REQUIRES esp_common freertos log
)
