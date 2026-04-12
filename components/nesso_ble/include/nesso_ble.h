/*
 * nesso_ble — BLE scanner + spam + tracker detection.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nesso_ble_init(void);
esp_err_t nesso_ble_deinit(void);
bool nesso_ble_is_ready(void);

/* -------------------- scanner -------------------- */

#define BLE_SCAN_MAX_DEVICES 32

typedef struct {
    uint8_t  addr[6];
    char     name[20];
    char     type[10];   /* "Apple", "Samsung", "AirTag", "HID", "BLE", etc. */
    int8_t   rssi;
    uint8_t  addr_type;
    bool     is_tracker; /* AirTag / SmartTag / Tile detected */
} nesso_ble_device_t;

typedef struct {
    nesso_ble_device_t devices[BLE_SCAN_MAX_DEVICES];
    size_t count;
} nesso_ble_scan_result_t;

/** Passive BLE scan. Sorted by RSSI descending. */
esp_err_t nesso_ble_scan(uint32_t duration_sec, nesso_ble_scan_result_t *out);

/* -------------------- spam -------------------- */

typedef enum {
    BLE_SPAM_APPLE = 0,
    BLE_SPAM_SAMSUNG,
    BLE_SPAM_GOOGLE,
    BLE_SPAM_WINDOWS,
    BLE_SPAM_ALL,
} nesso_ble_spam_type_t;

esp_err_t nesso_ble_spam_start(nesso_ble_spam_type_t type);
esp_err_t nesso_ble_spam_stop(void);
bool nesso_ble_spam_is_active(void);

/** Number of advertisements sent since spam started. */
uint32_t nesso_ble_spam_sent(void);

#ifdef __cplusplus
}
#endif
