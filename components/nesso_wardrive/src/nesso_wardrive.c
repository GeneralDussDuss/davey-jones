/*
 * nesso_wardrive.c — channel-hopping beacon logger.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nesso_wardrive.h"
#include "nesso_wifi.h"

static const char *TAG = "nesso_wdr";

/* SPIFFS mount params. Partition label matches partitions.csv. */
#define SPIFFS_LABEL     "storage"
#define SPIFFS_MOUNT     "/storage"

/* ----- module state ----- */

static nesso_wardrive_config_t s_cfg;
static bool                    s_running          = false;
static bool                    s_mounted_by_us    = false;

static TaskHandle_t            s_hop_task         = NULL;
static TaskHandle_t            s_log_task         = NULL;
static QueueHandle_t           s_log_queue        = NULL;
static SemaphoreHandle_t       s_lock             = NULL;
static nesso_wifi_promisc_sub_t s_sub_token       = 0;

static nesso_wardrive_ap_t    *s_aps              = NULL;
static size_t                  s_ap_count         = 0;
static size_t                  s_ap_capacity      = 0;
static uint32_t                s_beacons_parsed   = 0;
static uint32_t                s_packets_seen     = 0;
static uint32_t                s_csv_lines        = 0;
static uint32_t                s_gps_fixes_used   = 0;
static uint8_t                 s_current_channel  = 1;

/* ----- helpers ----- */

static inline uint32_t now_seconds(void)
{
    time_t t = time(NULL);
    if (t > 1600000000) return (uint32_t)t;
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static void format_timestamp(uint32_t ts, char *out, size_t n)
{
    time_t t = (time_t)ts;
    struct tm tmv;
    if (t > 1600000000) {
        gmtime_r(&t, &tmv);
        strftime(out, n, "%Y-%m-%d %H:%M:%S", &tmv);
    } else {
        snprintf(out, n, "boot+%lus", (unsigned long)ts);
    }
}

/* Linear search for a BSSID. Returns index or -1. */
static int find_ap(const uint8_t bssid[6])
{
    for (size_t i = 0; i < s_ap_count; ++i) {
        if (memcmp(s_aps[i].bssid, bssid, 6) == 0) return (int)i;
    }
    return -1;
}

/* Parse a beacon frame. Returns true if we pulled useful fields. */
static bool parse_beacon(const uint8_t *buf, uint16_t len,
                         nesso_wardrive_ap_t *out)
{
    if (len < 24 + 12 + 2) return false;
    if (buf[0] != 0x80) return false;

    memcpy(out->bssid, buf + 16, 6);

    uint16_t cap = (uint16_t)buf[34] | ((uint16_t)buf[35] << 8);
    out->privacy = (cap & 0x0010) != 0;

    /* Walk tagged parameters starting at offset 36. */
    size_t p = 36;
    out->ssid[0]         = '\0';
    out->primary_channel = 0;
    bool ssid_found      = false;
    bool has_rsn         = false;
    bool has_wpa         = false;

    while (p + 2 <= len) {
        uint8_t tag = buf[p];
        uint8_t tlen = buf[p + 1];
        if (p + 2 + tlen > len) break;

        switch (tag) {
        case 0: /* SSID — only take the first non-empty occurrence. */
            if (!ssid_found && tlen > 0 && tlen <= 32) {
                memcpy(out->ssid, buf + p + 2, tlen);
                out->ssid[tlen] = '\0';
                ssid_found = true;
            }
            break;
        case 3: /* DS parameter set — operating channel. */
            if (tlen == 1) {
                out->primary_channel = buf[p + 2];
            }
            break;
        case 48: /* RSN IE — indicates WPA2/WPA3. */
            has_rsn = true;
            break;
        case 221: /* Vendor IE — check for Microsoft WPA OUI. */
            if (tlen >= 4 &&
                buf[p+2] == 0x00 && buf[p+3] == 0x50 &&
                buf[p+4] == 0xF2 && buf[p+5] == 0x01) {
                has_wpa = true;
            }
            break;
        }
        p += 2 + tlen;
    }

    /* Derive auth hint from what we found. */
    if (!out->privacy) {
        memcpy(out->auth, "OPN", 4);
    } else if (has_rsn) {
        memcpy(out->auth, "WPA2", 5);
    } else if (has_wpa) {
        memcpy(out->auth, "WPA", 4);
    } else {
        memcpy(out->auth, "WEP", 4);
    }

    return true;
}

static const char *auth_str(const nesso_wardrive_ap_t *ap)
{
    return ap->auth[0] ? ap->auth : "???";
}

/* Emit one CSV line. Called only from the logger task. */
static void write_csv_line(FILE *f, const nesso_wardrive_ap_t *ap)
{
    char ts[32];
    format_timestamp(ap->first_seen_s, ts, sizeof(ts));

    nesso_gps_fix_t fix = {0};
    if (s_cfg.gps_cb) {
        s_cfg.gps_cb(&fix, s_cfg.gps_user);
        if (fix.has_fix) s_gps_fixes_used++;
    }

    fprintf(f,
            "%02X:%02X:%02X:%02X:%02X:%02X,"
            "\"%s\","
            "[%s],"
            "%s,"
            "%u,"
            "%d,"
            "%.6f,"
            "%.6f,"
            "%.2f,"
            "%.2f,"
            "WIFI\n",
            ap->bssid[0], ap->bssid[1], ap->bssid[2],
            ap->bssid[3], ap->bssid[4], ap->bssid[5],
            ap->ssid,
            auth_str(ap),
            ts,
            (unsigned)ap->primary_channel,
            (int)ap->rssi_peak,
            fix.latitude, fix.longitude,
            (double)fix.altitude_m, (double)fix.accuracy_m);
    fflush(f);
    s_csv_lines++;
}

/* ----- promiscuous callback ----- */

static void IRAM_ATTR wdr_rx_cb(const uint8_t *buf, uint16_t len,
                                 int8_t rssi, uint8_t channel, void *user)
{
    (void)user;
    s_packets_seen++;
    if (len < 36) return;

    nesso_wardrive_ap_t parsed = {0};
    if (!parse_beacon(buf, len, &parsed)) return;
    s_beacons_parsed++;

    if (parsed.primary_channel == 0) parsed.primary_channel = channel;
    if (parsed.primary_channel == 0) parsed.primary_channel = s_current_channel;
    parsed.rssi_last = rssi;

    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }

    int idx = find_ap(parsed.bssid);
    if (idx >= 0) {
        nesso_wardrive_ap_t *e = &s_aps[idx];
        e->last_seen_s = now_seconds();
        e->rssi_last   = rssi;
        if (rssi > e->rssi_peak) e->rssi_peak = rssi;
        if (e->ssid[0] == '\0' && parsed.ssid[0] != '\0') {
            memcpy(e->ssid, parsed.ssid, sizeof(e->ssid));
        }
        if (e->primary_channel == 0) e->primary_channel = parsed.primary_channel;
        if (e->auth[0] == '\0' || (e->auth[0] == '?' && parsed.auth[0] != '?')) {
            memcpy(e->auth, parsed.auth, sizeof(e->auth));
        }
        xSemaphoreGive(s_lock);
        return;
    }

    if (s_ap_count >= s_ap_capacity) {
        xSemaphoreGive(s_lock);
        return;
    }
    nesso_wardrive_ap_t *e = &s_aps[s_ap_count++];
    memcpy(e->bssid, parsed.bssid, 6);
    memcpy(e->ssid,  parsed.ssid,  sizeof(e->ssid));
    memcpy(e->auth,  parsed.auth,  sizeof(e->auth));
    e->primary_channel = parsed.primary_channel;
    e->rssi_last       = rssi;
    e->rssi_peak       = rssi;
    e->privacy         = parsed.privacy;
    e->first_seen_s    = now_seconds();
    e->last_seen_s     = e->first_seen_s;

    nesso_wardrive_ap_t snap = *e;
    xSemaphoreGive(s_lock);

    (void)xQueueSend(s_log_queue, &snap, 0);
}

/* ----- channel lock (for deauth mode) ----- */

static volatile uint8_t s_locked_channel = 0;

void nesso_wardrive_lock_channel(uint8_t ch)
{
    s_locked_channel = ch;
    if (ch != 0) {
        nesso_wifi_set_channel(ch);
        s_current_channel = ch;
    }
}

/* ----- channel hop task ----- */

static void hop_task(void *arg)
{
    (void)arg;
    uint8_t ch = 1;
    const TickType_t period = pdMS_TO_TICKS(s_cfg.dwell_ms);
    while (s_running) {
        if (s_locked_channel) {
            vTaskDelay(period);
            continue;
        }
        if (nesso_wifi_set_channel(ch) == ESP_OK) {
            s_current_channel = ch;
        }
        vTaskDelay(period);
        ch = (ch >= NESSO_WIFI_CHAN_MAX) ? NESSO_WIFI_CHAN_MIN : (uint8_t)(ch + 1);
    }
    s_hop_task = NULL;
    vTaskDelete(NULL);
}

/* ----- logger task ----- */

static void log_task(void *arg)
{
    char *path = (char *)arg;
    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed, logger task exiting", path);
        free(path);
        s_log_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) {
        fprintf(f,
            "WigleWifi-1.4,appRelease=davey_jones_0.1,model=nesso_n1,"
            "release=esp-idf,device=esp32c6,display=lcd,board=nesso_n1,"
            "brand=arduino\n"
            "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,"
            "CurrentLatitude,CurrentLongitude,AltitudeMeters,"
            "AccuracyMeters,Type\n");
        fflush(f);
    }

    nesso_wardrive_ap_t snap;
    while (s_running) {
        if (xQueueReceive(s_log_queue, &snap, pdMS_TO_TICKS(500)) == pdTRUE) {
            write_csv_line(f, &snap);
        }
    }

    fclose(f);
    free(path);
    s_log_task = NULL;
    vTaskDelete(NULL);
}

/* ----- SPIFFS mount ----- */

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
        ESP_LOGE(TAG, "spiffs register (%s): %s", SPIFFS_LABEL, esp_err_to_name(err));
        return err;
    }
    s_mounted_by_us = true;
    size_t total = 0, used = 0;
    if (esp_spiffs_info(SPIFFS_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "spiffs mounted: %zu/%zu KB used", used / 1024, total / 1024);
    }
    return ESP_OK;
}

/* ----- public lifecycle ----- */

esp_err_t nesso_wardrive_start(const nesso_wardrive_config_t *cfg)
{
    if (s_running) return ESP_OK;

    s_cfg = cfg ? *cfg : (nesso_wardrive_config_t)NESSO_WARDRIVE_CONFIG_DEFAULTS();
    if (s_cfg.dwell_ms == 0) s_cfg.dwell_ms = 500;
    if (s_cfg.max_aps  == 0) s_cfg.max_aps  = 256;

    const char *path_stack = s_cfg.csv_path ? s_cfg.csv_path : "/storage/wardrive.csv";
    char *path_owned = strdup(path_stack);
    if (!path_owned) return ESP_ERR_NO_MEM;

    ESP_RETURN_ON_ERROR(ensure_spiffs_mounted(), TAG, "spiffs mount");

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) { free(path_owned); return ESP_ERR_NO_MEM; }

    s_log_queue = xQueueCreate(16, sizeof(nesso_wardrive_ap_t));
    if (!s_log_queue) { free(path_owned); vSemaphoreDelete(s_lock); return ESP_ERR_NO_MEM; }

    s_aps = calloc(s_cfg.max_aps, sizeof(*s_aps));
    if (!s_aps) {
        free(path_owned);
        vQueueDelete(s_log_queue);
        vSemaphoreDelete(s_lock);
        return ESP_ERR_NO_MEM;
    }
    s_ap_capacity    = s_cfg.max_aps;
    s_ap_count       = 0;
    s_beacons_parsed = 0;
    s_packets_seen   = 0;
    s_csv_lines      = 0;
    s_gps_fixes_used = 0;

    ESP_RETURN_ON_ERROR(nesso_wifi_promisc_add_subscriber(wdr_rx_cb, NULL,
                                                          0,
                                                          &s_sub_token),
                        TAG, "promisc add subscriber");

    s_running = true;

    BaseType_t ok;
    ok = xTaskCreate(hop_task, "wdr_hop", 3072, NULL, 5, &s_hop_task);
    if (ok != pdPASS) goto fail;

    ok = xTaskCreate(log_task, "wdr_log", 4096, path_owned, 4, &s_log_task);
    if (ok != pdPASS) {
        /* hop_task is running — tell it to stop. */
        s_running = false;
        for (int i = 0; i < 20 && s_hop_task; ++i) vTaskDelay(pdMS_TO_TICKS(50));
        goto fail;
    }
    ESP_LOGI(TAG, "wardrive running: dwell=%ums cap=%zu",
             (unsigned)s_cfg.dwell_ms, s_ap_capacity);
    path_owned = NULL;  /* ownership transferred to log_task — don't free in fail */
    return ESP_OK;

fail:
    s_running = false;
    if (s_sub_token) {
        nesso_wifi_promisc_remove_subscriber(s_sub_token);
        s_sub_token = 0;
    }
    free(s_aps); s_aps = NULL;
    vQueueDelete(s_log_queue); s_log_queue = NULL;
    vSemaphoreDelete(s_lock);  s_lock = NULL;
    free(path_owned);
    return ESP_ERR_NO_MEM;
}

esp_err_t nesso_wardrive_stop(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;

    if (s_sub_token) {
        nesso_wifi_promisc_remove_subscriber(s_sub_token);
        s_sub_token = 0;
    }

    for (int i = 0; i < 20 && (s_hop_task || s_log_task); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_log_queue) { vQueueDelete(s_log_queue); s_log_queue = NULL; }
    if (s_lock)      { vSemaphoreDelete(s_lock);  s_lock      = NULL; }
    if (s_aps)       { free(s_aps); s_aps = NULL; s_ap_count = 0; s_ap_capacity = 0; }

    if (s_mounted_by_us) {
        esp_vfs_spiffs_unregister(SPIFFS_LABEL);
        s_mounted_by_us = false;
    }
    return ESP_OK;
}

/* ----- public status ----- */

esp_err_t nesso_wardrive_status(nesso_wardrive_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    out->current_channel   = s_current_channel;
    out->total_aps         = s_ap_count;
    out->beacons_parsed    = s_beacons_parsed;
    out->packets_seen      = s_packets_seen;
    out->csv_lines_written = s_csv_lines;
    out->gps_fixes_used    = s_gps_fixes_used;
    return ESP_OK;
}

esp_err_t nesso_wardrive_snapshot(nesso_wardrive_ap_t *out,
                                  size_t max, size_t *out_count)
{
    if (!out || !out_count) return ESP_ERR_INVALID_ARG;
    if (!s_lock) { *out_count = 0; return ESP_OK; }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_ap_count < max ? s_ap_count : max;
    memcpy(out, s_aps, n * sizeof(*s_aps));
    xSemaphoreGive(s_lock);
    *out_count = n;
    return ESP_OK;
}
