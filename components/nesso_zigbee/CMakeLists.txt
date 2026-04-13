idf_component_register(
    SRCS         "src/nesso_zigbee.c"
    INCLUDE_DIRS "include"
    REQUIRES     ieee802154
    PRIV_REQUIRES esp_common freertos log esp_timer spiffs
)
