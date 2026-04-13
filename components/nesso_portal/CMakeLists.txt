idf_component_register(
    SRCS         "src/nesso_portal.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_wifi esp_http_server esp_netif esp_event
    PRIV_REQUIRES esp_common freertos log lwip spiffs
)
