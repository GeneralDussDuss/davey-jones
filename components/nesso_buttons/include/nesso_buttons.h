/*
 * nesso_buttons — debounced KEY1/KEY2 events for the Nesso N1.
 *
 * Background task polls expander E0 at 20 Hz (50 ms), confirms each state
 * change across two consecutive samples, and pushes press/release/long-press
 * events onto a queue that the UI task drains. Keeps KEY1/KEY2 handling off
 * the hot path for WiFi / LoRa work.
 *
 * Why not interrupts? The PI4IOE5V6408's INT line isn't exposed to the ESP
 * on the Nesso — the chip's output pin isn't routed. Polling is the only
 * option. 20 Hz is imperceptibly snappy and costs ~1 I²C transaction per
 * tick (one read of INPUT_STATUS for the whole port).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NESSO_KEY1 = 0,
    NESSO_KEY2 = 1,
} nesso_key_t;

typedef enum {
    NESSO_BTN_EVT_PRESS      = 0, /* transition idle -> held */
    NESSO_BTN_EVT_RELEASE    = 1, /* transition held -> idle */
    NESSO_BTN_EVT_LONG_PRESS = 2, /* held continuously for ≥ long-press threshold */
} nesso_btn_event_type_t;

typedef struct {
    nesso_key_t            key;
    nesso_btn_event_type_t type;
    uint32_t               held_ms;  /* duration the key was held (release only) */
} nesso_btn_event_t;

/* Timing defaults, override via nesso_buttons_start(). */
#define NESSO_BUTTONS_POLL_MS_DEFAULT        50   /* 20 Hz */
#define NESSO_BUTTONS_LONG_PRESS_MS_DEFAULT  600  /* 0.6 s */

typedef struct {
    uint32_t poll_interval_ms;  /* 0 = NESSO_BUTTONS_POLL_MS_DEFAULT */
    uint32_t long_press_ms;     /* 0 = NESSO_BUTTONS_LONG_PRESS_MS_DEFAULT */
    uint32_t queue_depth;       /* 0 = 16 */
} nesso_buttons_config_t;

/**
 * Spin up the polling task. Requires nesso_bsp_init() to have run.
 * Safe to call multiple times — subsequent calls return ESP_OK without
 * starting a second task.
 */
esp_err_t nesso_buttons_start(const nesso_buttons_config_t *cfg);

/** Stop the task and free the queue. Drops any queued events. */
esp_err_t nesso_buttons_stop(void);

/**
 * The queue the UI task should xQueueReceive() from.
 * Returns NULL if nesso_buttons_start() hasn't run.
 */
QueueHandle_t nesso_buttons_event_queue(void);

/**
 * Non-event snapshot — useful for polling-style UI code that doesn't
 * want to drain the queue. Reads the latched last-known state, not the
 * raw expander, so it's cheap.
 */
bool nesso_buttons_is_held(nesso_key_t key);

#ifdef __cplusplus
}
#endif
