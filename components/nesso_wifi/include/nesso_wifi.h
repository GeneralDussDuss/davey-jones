/*
 * nesso_wifi — offensive WiFi surface for DAVEY JONES.
 *
 * Wraps ESP32-C6's esp_wifi APIs into an init / scan / promiscuous / raw-TX
 * surface we'll build real features on top of (wardriving, deauth, beacon
 * spam, evil portal, EAPOL capture, etc.).
 *
 * Why a wrapper at all: esp_wifi has several ways to do the same thing,
 * some of them subtly broken when combined (e.g. scan while promiscuous,
 * or raw TX without a started radio). This component encodes the
 * "known-good" sequences so no feature needs to rediscover them.
 *
 * ESP32-C6 notes:
 *   - HE frames (WiFi 6) work by default, older recon tools targeting
 *     802.11n still work against most APs.
 *   - Power save forced OFF — all the fun needs constant RX.
 *   - Storage forced to RAM so scans don't pound NVS flash.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32-C6 is 2.4 GHz only — 1..13 in most regions, 1..11 US. */
#define NESSO_WIFI_CHAN_MIN 1
#define NESSO_WIFI_CHAN_MAX 13

/**
 * One scan result entry, trimmed to the fields Davey Jones actually uses
 * vs. esp_wifi's larger wifi_ap_record_t.
 */
typedef struct {
    uint8_t  bssid[6];
    char     ssid[33];        /* 32 bytes + NUL */
    uint8_t  primary_channel;
    int8_t   rssi;
    uint32_t auth_mode;       /* wifi_auth_mode_t — stored as u32 to avoid pulling the enum */
    bool     hidden;
} nesso_wifi_ap_t;

/**
 * Promiscuous-mode packet callback. `buf` is the 802.11 payload exactly
 * as delivered by esp_wifi's hook; `len` is the total size. `rssi` is
 * pulled from the rx_ctrl structure so the callback doesn't have to know
 * about wifi_promiscuous_pkt_t internals.
 *
 * Runs in WiFi task context — keep it short, post to a queue for real work.
 */
typedef void (*nesso_wifi_packet_cb_t)(const uint8_t *buf, uint16_t len,
                                        int8_t rssi, uint8_t channel,
                                        void *user);

/* -------------------- lifecycle -------------------- */

/**
 * Initialize WiFi in STA mode with settings tuned for offensive use:
 *   - storage = RAM (no NVS writes on scan)
 *   - power-save off
 *   - country = worldwide / permissive channel list
 *   - esp_netif + event loop created if not already
 *
 * Safe to call multiple times; second call is a no-op.
 */
esp_err_t nesso_wifi_init(void);

/** Tear down WiFi, event loop, netif. */
esp_err_t nesso_wifi_deinit(void);

/* -------------------- scan -------------------- */

/**
 * Active scan across all 2.4 GHz channels, including hidden SSIDs.
 * Blocks until the scan completes. `out` may be NULL to just count.
 *
 * @param out          Caller-allocated array for results (may be NULL).
 * @param max_results  Capacity of `out`.
 * @param out_count    Number of APs actually returned (≤ max_results).
 */
esp_err_t nesso_wifi_scan(nesso_wifi_ap_t *out, size_t max_results,
                          size_t *out_count);

/* -------------------- promiscuous / channel -------------------- */

/** Set the current listening channel. Requires init but not promisc-on. */
esp_err_t nesso_wifi_set_channel(uint8_t channel);

/** Read back the current channel (1..13). */
esp_err_t nesso_wifi_get_channel(uint8_t *out_channel);

/**
 * Enter promiscuous mode with the given callback. Any previously-attached
 * callback is replaced. Pass cb=NULL + stop later.
 *
 * @param cb        Callback fired for every captured frame.
 * @param user      Opaque pointer handed to the callback.
 * @param filter    Bitmask of WIFI_PROMIS_FILTER_MASK_* — 0 means "all".
 */
esp_err_t nesso_wifi_promisc_start(nesso_wifi_packet_cb_t cb, void *user,
                                    uint32_t filter);

/** Leave promiscuous mode, drop callback. */
esp_err_t nesso_wifi_promisc_stop(void);

/** True if currently in promiscuous mode. */
bool nesso_wifi_promisc_is_on(void);

/* -------------------- multi-subscriber promisc fanout -------------------- */

/*
 * nesso_wifi supports up to NESSO_WIFI_MAX_PROMISC_SUBS simultaneous
 * callbacks. nesso_wifi_promisc_start() is a convenience wrapper that
 * installs a single subscriber and enables promiscuous mode — but if
 * you need wardrive + eapol + something else all listening at once, use
 * the subscriber API directly.
 *
 * Filters are ORed across subscribers: whatever frames each subscriber
 * asks for, the union of those goes to the radio; each subscriber still
 * sees everything in the ORed set.
 *
 * Promiscuous mode is enabled automatically when the first subscriber
 * registers and disabled when the last one unregisters.
 */

#define NESSO_WIFI_MAX_PROMISC_SUBS 4

/** Opaque subscriber token. Non-zero on success; 0 means "all slots full". */
typedef int nesso_wifi_promisc_sub_t;

esp_err_t nesso_wifi_promisc_add_subscriber(nesso_wifi_packet_cb_t cb,
                                             void *user,
                                             uint32_t filter,
                                             nesso_wifi_promisc_sub_t *out_token);

esp_err_t nesso_wifi_promisc_remove_subscriber(nesso_wifi_promisc_sub_t token);

/* -------------------- raw 802.11 TX -------------------- */

/**
 * Inject a raw 802.11 frame. Radio must be started; promiscuous mode
 * is not required but does not hurt.
 *
 * The driver will set/increment the sequence number for you unless you
 * pass use_own_seq=true.
 *
 * @param frame        Full 802.11 frame starting at frame control.
 * @param len          Frame length in bytes (24..1500 typical).
 * @param use_own_seq  If true, driver honors seq_ctrl from the frame buffer.
 */
esp_err_t nesso_wifi_raw_tx(const uint8_t *frame, size_t len, bool use_own_seq);

/**
 * Send a deauth frame. If target_mac is NULL, uses broadcast
 * (FF:FF:FF:FF:FF:FF) to kick every client of the AP.
 *
 * @param ap_bssid    6-byte AP MAC.
 * @param target_mac  6-byte client MAC, or NULL for broadcast.
 * @param reason      802.11 reason code (1 = unspecified, 7 = class-3 frame from non-assoc).
 * @param count       How many times to send (spam helper; use 1 for one frame).
 */
esp_err_t nesso_wifi_send_deauth(const uint8_t ap_bssid[6],
                                  const uint8_t target_mac[6],
                                  uint16_t reason,
                                  uint16_t count);

/* -------------------- beacon spam -------------------- */

/**
 * Start broadcasting fake beacon frames with the given SSIDs.
 * Spawns a task that cycles through the SSID list, sending one beacon
 * per SSID per cycle at ~10ms intervals. Call nesso_wifi_beacon_spam_stop()
 * to stop.
 *
 * @param ssids      Array of SSID strings.
 * @param count      Number of SSIDs.
 * @param channel    Channel to broadcast on (0 = current).
 */
esp_err_t nesso_wifi_beacon_spam_start(const char **ssids, size_t count,
                                        uint8_t channel);

/** Stop beacon spam. */
esp_err_t nesso_wifi_beacon_spam_stop(void);

/** True if beacon spam is currently active. */
bool nesso_wifi_beacon_spam_is_active(void);

#ifdef __cplusplus
}
#endif
