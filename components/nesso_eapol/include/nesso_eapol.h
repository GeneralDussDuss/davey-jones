/*
 * nesso_eapol — WPA2/WPA3 PMKID + handshake capture for DAVEY JONES.
 *
 * Sits on top of nesso_wifi's promiscuous mode. Parses 802.11 data frames
 * for EAPOL-key messages and extracts PMKIDs (RSN PMKID KDE in key data),
 * which is the fastest path to an offline WPA2-PSK crackable blob.
 *
 * Output: /storage/handshakes.hc22000 (hashcat mode 22000 format).
 *
 * Known limits (intentional, first version):
 *   - PMKID only, no full 4-way handshake M1+M2 MIC capture. ~70% of
 *     APs leak a PMKID in M1; that's enough for most real-world cracks.
 *   - No WPA3 SAE downgrade detection yet.
 *
 * Coexistence: uses the nesso_wifi subscriber API, so it can run in
 * parallel with nesso_wardrive and any other promiscuous consumers.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Optional SSID lookup — if registered, called when writing a .hc22000
 * line for a BSSID whose SSID the component doesn't know yet. Lets you
 * source SSIDs from nesso_wardrive's snapshot or an external database.
 *
 * Write up to `out_cap-1` bytes into `out_ssid` and NUL-terminate.
 * Leave out_ssid[0]='\0' if unknown — the record will still be written
 * with empty ESSID, but hashcat can't crack it without one.
 */
typedef void (*nesso_eapol_ssid_lookup_cb_t)(const uint8_t bssid[6],
                                               char  *out_ssid,
                                               size_t out_cap,
                                               void  *user);

typedef struct {
    const char *output_path;     /* NULL → "/storage/handshakes.hc22000" */
    size_t      max_seen;        /* BSSID dedup capacity. Default 128. */
    nesso_eapol_ssid_lookup_cb_t ssid_cb;
    void       *ssid_user;
} nesso_eapol_config_t;

#define NESSO_EAPOL_CONFIG_DEFAULTS() ((nesso_eapol_config_t){ \
    .output_path = NULL,     \
    .max_seen    = 128,      \
    .ssid_cb     = NULL,     \
    .ssid_user   = NULL,     \
})

/* -------------------- lifecycle -------------------- */

/** Start promiscuous capture + EAPOL parsing. Requires nesso_wifi_init(). */
esp_err_t nesso_eapol_start(const nesso_eapol_config_t *cfg);

/** Stop, flush, unmount-if-we-mounted. */
esp_err_t nesso_eapol_stop(void);

/* -------------------- status -------------------- */

typedef struct {
    uint32_t packets_seen;
    uint32_t data_frames;
    uint32_t eapol_frames;
    uint32_t pmkids_captured;
    uint32_t beacons_indexed;
    uint32_t ssids_known;
    uint32_t hc22000_lines_written;
} nesso_eapol_status_t;

esp_err_t nesso_eapol_status(nesso_eapol_status_t *out);

#ifdef __cplusplus
}
#endif
