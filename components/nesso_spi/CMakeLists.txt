idf_component_register(
    SRCS         "src/nesso_spi.c"
    INCLUDE_DIRS "include"
    REQUIRES     driver nesso_bsp
    PRIV_REQUIRES esp_common log
)
