/*
 * nesso_ui — LVGL status screen for DAVEY JONES.
 *
 * Sits on top of esp_lvgl_port (managed component) which handles the LVGL
 * task, tick source, double buffer, RGB565 byte swap, and panel flush. We
 * just build a screen of labels and drive their text from our counter
 * APIs via an LVGL timer.
 *
 * Palette: D (RGB565-exact) — black bg, cyan body, magenta accents,
 * yellow for alerts/values.
 *
 * Dependencies: nesso_lcd must be initialized first (we grab the panel
 * handles from it).
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NESSO_UI_VIEW_DASH = 0,    /* live counters */
    NESSO_UI_VIEW_APS,         /* top-RSSI AP list (placeholder) */
    NESSO_UI_VIEW_LORA,        /* LoRa radio state (placeholder) */
    NESSO_UI_VIEW_COUNT,
} nesso_ui_view_t;

/**
 * Bring up LVGL via esp_lvgl_port, create the dashboard screen, kick off
 * the 250 ms status refresh timer, and start consuming nesso_buttons
 * events to cycle views.
 *
 * Requires: nesso_bsp, nesso_spi, nesso_lcd, nesso_buttons already up.
 */
esp_err_t nesso_ui_init(void);

/** Tear down the UI. Does not deinit LVGL since esp_lvgl_port manages it. */
esp_err_t nesso_ui_deinit(void);

/** Switch to a named view immediately. */
esp_err_t nesso_ui_show(nesso_ui_view_t view);

/** True if nesso_ui_init has completed successfully. */
bool nesso_ui_is_up(void);

#ifdef __cplusplus
}
#endif
