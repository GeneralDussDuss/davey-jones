/*
 * nesso_ble — BLE scanner + spam for DAVEY JONES.
 *
 * ESP32-C6 has BLE 5.0 only (no Classic Bluetooth).
 * Scanner: discover nearby BLE devices with name/MAC/RSSI.
 * Spam: broadcast fake pairing notifications to nearby phones.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------- init -------------------- */

esp_err_t nesso_ble_init(void);
esp_err_t nesso_ble_deinit(void);
bool nesso_ble_is_ready(void);

/* -------------------- scanner -------------------- */

#define BLE_SCAN_MAX_DEVICES 32

typedef struct {
    uint8_t  addr[6];
    char     name[20];   /* truncated if longer */
    int8_t   rssi;
    uint8_t  addr_type;  /* 0=public, 1=random */
} nesso_ble_device_t;

typedef struct {
    nesso_ble_device_t devices[BLE_SCAN_MAX_DEVICES];
    size_t count;
} nesso_ble_scan_result_t;

/**
 * Start a BLE scan for the given duration. Blocks until complete.
 * Results are deduped by address.
 */
esp_err_t nesso_ble_scan(uint32_t duration_sec, nesso_ble_scan_result_t *out);

/* -------------------- spam -------------------- */

typedef enum {
    BLE_SPAM_APPLE = 0,    /* Fake AirPod/Beats pairing popups on iPhones */
    BLE_SPAM_SAMSUNG,      /* Samsung SmartTag notifications */
    BLE_SPAM_GOOGLE,       /* Google Fast Pair popups on Android */
    BLE_SPAM_WINDOWS,      /* Windows Swift Pair notifications */
    BLE_SPAM_ALL,          /* Cycle through all of the above */
} nesso_ble_spam_type_t;

/** Start broadcasting spam advertisements. Non-blocking — runs in background. */
esp_err_t nesso_ble_spam_start(nesso_ble_spam_type_t type);

/** Stop spam. */
esp_err_t nesso_ble_spam_stop(void);

/** True if spam is currently running. */
bool nesso_ble_spam_is_active(void);

#ifdef __cplusplus
}
#endif
