/*
 * nesso_wardrive — channel-hopping beacon logger for DAVEY JONES.
 *
 * Sits on top of nesso_wifi in promiscuous mode. Hops 2.4 GHz channels
 * 1..13, parses beacon frames, dedupes APs in memory, and streams new
 * observations to a Wigle-format CSV file on the SPIFFS "storage" partition.
 *
 * Design choices:
 *   - Parsing runs in the WiFi task context via the promisc callback —
 *     cheap memcpy + memcmp + array scan, no allocation, no file IO.
 *   - New-AP notifications are pushed onto a queue; a dedicated logger
 *     task does the slow file write so the WiFi callback never blocks.
 *   - GPS is an optional pluggable callback. If you don't register one,
 *     all records log lat/lon as 0. Wire it up once you add a u-blox
 *     module on the Grove port.
 *
 * Wigle CSV format reference: https://api.wigle.net/uploads.html
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------- optional GPS hook -------------------- */

typedef struct {
    double latitude;
    double longitude;
    float  altitude_m;
    float  accuracy_m;
    bool   has_fix;
} nesso_gps_fix_t;

/** Called once per NEW AP (dedupe miss). Fill *out with a current fix. */
typedef void (*nesso_gps_fetch_cb_t)(nesso_gps_fix_t *out, void *user);

/* -------------------- config -------------------- */

typedef struct {
    /* CSV file path. NULL defaults to "/storage/wardrive.csv". Component
     * auto-mounts SPIFFS on the "storage" partition if not already mounted. */
    const char *csv_path;

    /* Channel dwell time in ms. Default 500 ms. */
    uint32_t dwell_ms;

    /* Max APs held in RAM. Default 256. */
    size_t   max_aps;

    /* Optional GPS fetcher — called on every new-AP write. */
    nesso_gps_fetch_cb_t gps_cb;
    void                *gps_user;
} nesso_wardrive_config_t;

#define NESSO_WARDRIVE_CONFIG_DEFAULTS() ((nesso_wardrive_config_t){ \
    .csv_path = NULL,      \
    .dwell_ms = 500,       \
    .max_aps  = 256,       \
    .gps_cb   = NULL,      \
    .gps_user = NULL,      \
})

/* -------------------- lifecycle -------------------- */

/**
 * Start the wardrive pipeline: SPIFFS mount (if needed), CSV header,
 * channel-hop task, logger task, promiscuous RX with mgmt-frame filter.
 *
 * Requires nesso_wifi_init() to have run first.
 * Safe to call twice — second call returns ESP_OK without duplicating state.
 */
esp_err_t nesso_wardrive_start(const nesso_wardrive_config_t *cfg);

/** Stop hopping + logging. Closes the CSV file, frees state. */
esp_err_t nesso_wardrive_stop(void);

/* -------------------- observation access -------------------- */

typedef struct {
    uint8_t  bssid[6];
    char     ssid[33];
    char     auth[5];       /* "OPN", "WEP", "WPA", "WPA2" */
    uint8_t  primary_channel;
    int8_t   rssi_peak;
    int8_t   rssi_last;
    uint32_t first_seen_s;  /* seconds since boot or epoch if RTC set */
    uint32_t last_seen_s;
    bool     privacy;       /* capability info bit 4 — privacy required */
} nesso_wardrive_ap_t;

typedef struct {
    size_t   total_aps;
    uint8_t  current_channel;
    uint32_t beacons_parsed;
    uint32_t packets_seen;
    uint32_t csv_lines_written;
    uint32_t gps_fixes_used;
} nesso_wardrive_status_t;

/** Populate *out with live counters. */
esp_err_t nesso_wardrive_status(nesso_wardrive_status_t *out);

/**
 * Lock the channel-hop task to a specific channel. Pass ch=0 to resume
 * normal hopping. Used by the deauth attack mode to stay on the target
 * AP's channel while waiting for EAPOL reconnection.
 */
void nesso_wardrive_lock_channel(uint8_t ch);

/**
 * Copy up to `max` observed APs into `out`. Useful for a UI snapshot.
 * `out_count` receives the number of entries actually copied.
 */
esp_err_t nesso_wardrive_snapshot(nesso_wardrive_ap_t *out,
                                  size_t max, size_t *out_count);

#ifdef __cplusplus
}
#endif
