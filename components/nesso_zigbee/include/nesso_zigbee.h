/*
 * nesso_zigbee — 802.15.4 Zigbee/Thread sniffer for DAVEY JONES.
 *
 * The ESP32-C6 has a built-in IEEE 802.15.4 radio — one of the few
 * cheap dev boards that can natively sniff Zigbee, Thread, and Matter
 * traffic. This component uses the esp_ieee802154 driver for passive
 * packet capture across all 16 channels (11-26).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZIGBEE_CHAN_MIN 11
#define ZIGBEE_CHAN_MAX 26
#define ZIGBEE_MAX_DEVICES 32

typedef struct {
    uint16_t short_addr;
    uint16_t pan_id;
    int8_t   rssi;
    uint8_t  channel;
    char     type[8];    /* "Zigbee", "Thread", "802154" */
    uint32_t last_seen;
} nesso_zigbee_device_t;

typedef struct {
    nesso_zigbee_device_t devices[ZIGBEE_MAX_DEVICES];
    size_t count;
    uint32_t packets_seen;
    uint8_t  current_channel;
} nesso_zigbee_scan_t;

/** Initialize the 802.15.4 radio for sniffing. */
esp_err_t nesso_zigbee_init(void);
esp_err_t nesso_zigbee_deinit(void);
bool nesso_zigbee_is_ready(void);

/** Start channel-hopping sniffer. Results accumulate in scan state. */
esp_err_t nesso_zigbee_scan_start(void);
esp_err_t nesso_zigbee_scan_stop(void);
bool nesso_zigbee_scan_is_active(void);

/** Get current scan results. */
esp_err_t nesso_zigbee_scan_get(nesso_zigbee_scan_t *out);

/** Log all packets to SPIFFS CSV. */
esp_err_t nesso_zigbee_log_start(const char *path);
esp_err_t nesso_zigbee_log_stop(void);
uint32_t nesso_zigbee_log_count(void);

#ifdef __cplusplus
}
#endif
