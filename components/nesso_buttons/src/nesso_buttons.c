/*
 * nesso_buttons.c — KEY1/KEY2 polling + debouncing for the Nesso N1.
 */

#include "nesso_buttons.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nesso_bsp.h"
#include "pi4ioe5v6408.h"

static const char *TAG = "nesso_btn";

#define NUM_KEYS 2

typedef struct {
    bool     raw_last;     /* previous raw sample for two-sample confirmation */
    bool     confirmed;    /* debounced state: true = pressed */
    bool     long_fired;   /* long-press event already emitted for this hold */
    uint64_t down_time_us; /* esp_timer_get_time() at press */
} key_state_t;

static TaskHandle_t   s_task        = NULL;
static QueueHandle_t  s_event_queue = NULL;
static uint32_t       s_poll_ms     = NESSO_BUTTONS_POLL_MS_DEFAULT;
static uint32_t       s_long_ms     = NESSO_BUTTONS_LONG_PRESS_MS_DEFAULT;
static key_state_t    s_keys[NUM_KEYS];

/* E0 pin number for each logical key, in array order. */
static const uint8_t s_key_pins[NUM_KEYS] = {
    NESSO_E0_PIN_KEY1,
    NESSO_E0_PIN_KEY2,
};

static void emit(nesso_key_t key, nesso_btn_event_type_t type, uint32_t held_ms)
{
    nesso_btn_event_t evt = {
        .key     = key,
        .type    = type,
        .held_ms = held_ms,
    };
    /* Non-blocking send — if the UI task is slow we drop events rather than
     * blocking the polling task. Queue overruns are observable via logs. */
    if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropping %s on key%d",
                 type == NESSO_BTN_EVT_PRESS   ? "PRESS"
                 : type == NESSO_BTN_EVT_RELEASE ? "RELEASE"
                                                  : "LONG",
                 (int)key);
    }
}

/*
 * Process one raw port read and emit any resulting events for the given key.
 * Caller passes the raw pin state (true = currently held).
 */
static void step_key(nesso_key_t key, bool raw_now)
{
    key_state_t *k = &s_keys[key];

    /*
     * Two-sample confirmation. We only accept a new state once it's been
     * read twice in a row — buys us ~one poll interval of glitch rejection
     * (50 ms with the default rate) without any timestamping.
     */
    if (raw_now != k->confirmed && raw_now == k->raw_last) {
        k->confirmed = raw_now;

        if (raw_now) {
            /* Rising edge into held state. */
            k->down_time_us = esp_timer_get_time();
            k->long_fired   = false;
            emit(key, NESSO_BTN_EVT_PRESS, 0);
        } else {
            /* Falling edge into released state. */
            uint64_t held_us = esp_timer_get_time() - k->down_time_us;
            emit(key, NESSO_BTN_EVT_RELEASE, (uint32_t)(held_us / 1000ULL));
        }
    }

    /* Long-press detection — while still held, check duration. */
    if (k->confirmed && !k->long_fired) {
        uint64_t held_us = esp_timer_get_time() - k->down_time_us;
        if ((held_us / 1000ULL) >= s_long_ms) {
            k->long_fired = true;
            emit(key, NESSO_BTN_EVT_LONG_PRESS, (uint32_t)(held_us / 1000ULL));
        }
    }

    k->raw_last = raw_now;
}

static void buttons_task(void *arg)
{
    (void)arg;
    pi4ioe_handle_t e0 = nesso_expander_e0();
    if (!e0) {
        ESP_LOGE(TAG, "E0 handle is NULL; did nesso_bsp_init() run?");
        vTaskDelete(NULL);
        return;
    }

    const TickType_t period = pdMS_TO_TICKS(s_poll_ms);
    TickType_t last_wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "polling task up (poll=%ums, long-press=%ums)",
             (unsigned)s_poll_ms, (unsigned)s_long_ms);

    while (1) {
        uint8_t port = 0;
        if (pi4ioe_read_input_port(e0, &port) == ESP_OK) {
            /* Keys are active-low with pull-ups: bit clear → held. */
            for (int i = 0; i < NUM_KEYS; ++i) {
                bool raw_held = (port & (1U << s_key_pins[i])) == 0;
                step_key((nesso_key_t)i, raw_held);
            }
        } else {
            /* Don't spam — pi4ioe already logs. Just pause an extra beat. */
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vTaskDelayUntil(&last_wake, period);
    }
}

esp_err_t nesso_buttons_start(const nesso_buttons_config_t *cfg)
{
    if (s_task) {
        return ESP_OK;  /* already running */
    }

    uint32_t poll  = (cfg && cfg->poll_interval_ms) ? cfg->poll_interval_ms
                                                    : NESSO_BUTTONS_POLL_MS_DEFAULT;
    uint32_t longp = (cfg && cfg->long_press_ms)    ? cfg->long_press_ms
                                                    : NESSO_BUTTONS_LONG_PRESS_MS_DEFAULT;
    uint32_t depth = (cfg && cfg->queue_depth)      ? cfg->queue_depth
                                                    : 16;

    s_event_queue = xQueueCreate(depth, sizeof(nesso_btn_event_t));
    if (!s_event_queue) return ESP_ERR_NO_MEM;

    s_poll_ms = poll;
    s_long_ms = longp;
    memset(s_keys, 0, sizeof(s_keys));

    BaseType_t ok = xTaskCreate(buttons_task, "nesso_btn", 3072, NULL, 5, &s_task);
    if (ok != pdPASS) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t nesso_buttons_stop(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_event_queue) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
    memset(s_keys, 0, sizeof(s_keys));
    return ESP_OK;
}

QueueHandle_t nesso_buttons_event_queue(void)
{
    return s_event_queue;
}

bool nesso_buttons_is_held(nesso_key_t key)
{
    if ((int)key < 0 || (int)key >= NUM_KEYS) return false;
    return s_keys[key].confirmed;
}
