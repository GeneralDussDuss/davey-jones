/*
 * nesso_ui.c — LVGL dashboard + deauth attack mode for DAVEY JONES.
 *
 * Views:
 *   DASH  — live counters (APs, beacons, PMKIDs, channel, USB)
 *   APS   — top APs by RSSI with cursor selection + deauth trigger
 *   LORA  — placeholder
 *
 * Button mapping:
 *   DASH/LORA: KEY2 = next view
 *   APS:       KEY2 = scroll cursor down, KEY1 = deauth selected AP
 *   ANY:       KEY1 long-press = stop active deauth
 */

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "driver/i2c_master.h"
#include "esp_lcd_touch_ft5x06.h"

#include "nesso_bsp.h"
#include "nesso_buttons.h"
#include "nesso_lcd.h"
#include "nesso_ui.h"
#include "nesso_wardrive.h"
#include "nesso_eapol.h"
#include "nesso_wifi.h"

static const char *TAG = "nesso_ui";

/* -------------------- Palette D (RGB565-exact) -------------------- */

#define COL_BLACK   lv_color_hex(0x000000)
#define COL_CYAN    lv_color_hex(0x00FFFF)
#define COL_MAGENTA lv_color_hex(0xFF00FF)
#define COL_YELLOW  lv_color_hex(0xFFFF00)
#define COL_WHITE   lv_color_hex(0xFFFFFF)
#define COL_RED     lv_color_hex(0xFF0000)

/* -------------------- module state -------------------- */

static bool              s_up            = false;
static lv_display_t     *s_disp          = NULL;
static nesso_ui_view_t   s_view          = NESSO_UI_VIEW_DASH;

/* Dash view widgets */
static lv_obj_t *s_dash_scr        = NULL;
static lv_obj_t *s_title_label     = NULL;
static lv_obj_t *s_chan_label      = NULL;
static lv_obj_t *s_aps_label       = NULL;
static lv_obj_t *s_beacon_label    = NULL;
static lv_obj_t *s_pmkid_label     = NULL;
static lv_obj_t *s_power_label     = NULL;
static lv_obj_t *s_dash_view_label = NULL;

/* APS view */
static lv_obj_t *s_aps_scr         = NULL;
#define APS_VIEW_ROWS 8
static lv_obj_t *s_ap_rows[APS_VIEW_ROWS];
static lv_obj_t *s_aps_status_label = NULL;
static lv_obj_t *s_aps_view_label   = NULL;
static int       s_aps_cursor       = 0;

/* AP snapshot for the APS view. Refreshed periodically. */
static nesso_wardrive_ap_t s_ap_snap[APS_VIEW_ROWS];
static size_t              s_ap_snap_count = 0;

/* Deauth attack state */
static bool     s_deauth_active   = false;
static uint8_t  s_deauth_bssid[6] = {0};
static uint8_t  s_deauth_channel  = 0;
static char     s_deauth_ssid[33] = {0};
static uint32_t s_deauth_sent     = 0;
static lv_timer_t *s_deauth_timer = NULL;

/* LORA view */
static lv_obj_t *s_lora_scr = NULL;

static lv_timer_t   *s_refresh_timer = NULL;
static TaskHandle_t  s_button_task   = NULL;

/* Touch */
static esp_lcd_touch_handle_t s_touch = NULL;
static lv_indev_t            *s_touch_indev = NULL;

/* -------------------- widget helpers -------------------- */

static lv_obj_t *make_row_label(lv_obj_t *parent, int y, lv_color_t color,
                                 const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 4, y);
    return lbl;
}

/* -------------------- dashboard screen -------------------- */

static void build_dash_screen(void)
{
    s_dash_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_dash_scr, COL_BLACK, 0);
    lv_obj_set_style_bg_opa  (s_dash_scr, LV_OPA_COVER, 0);

    s_title_label = lv_label_create(s_dash_scr);
    lv_label_set_text(s_title_label, "DAVEY JONES");
    lv_obj_set_style_text_color(s_title_label, COL_MAGENTA, 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 4);

    int y = 28;
    s_chan_label    = make_row_label(s_dash_scr, y, COL_CYAN,  "CH   --"); y += 22;
    s_aps_label    = make_row_label(s_dash_scr, y, COL_CYAN,  "APs  0");  y += 22;
    s_beacon_label = make_row_label(s_dash_scr, y, COL_CYAN,  "BCN  0");  y += 22;
    s_pmkid_label  = make_row_label(s_dash_scr, y, COL_YELLOW,"PMK  0");  y += 22;
    s_power_label  = make_row_label(s_dash_scr, y, COL_CYAN,  "USB  ?");

    s_dash_view_label = lv_label_create(s_dash_scr);
    lv_obj_set_style_text_color(s_dash_view_label, COL_WHITE, 0);
    lv_label_set_text(s_dash_view_label, "[1/3] DASH");
    lv_obj_align(s_dash_view_label, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/* Forward declarations for deauth (defined below APS view). */
static void start_deauth(int ap_index);
static void stop_deauth(void);

/* -------------------- APS view (target selection + deauth) -------------------- */

/* Touch callback for tapping an AP row. User data = row index. */
static void ap_row_clicked(lv_event_t *e)
{
    int row = (int)(intptr_t)lv_event_get_user_data(e);
    if ((size_t)row >= s_ap_snap_count) return;

    s_aps_cursor = row;
    if (s_deauth_active) {
        stop_deauth();
    }
    start_deauth(row);
}

static void build_aps_screen(void)
{
    s_aps_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_aps_scr, COL_BLACK, 0);
    lv_obj_set_style_bg_opa  (s_aps_scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(s_aps_scr);
    lv_label_set_text(title, "SELECT TARGET");
    lv_obj_set_style_text_color(title, COL_MAGENTA, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    /* 8 rows for AP entries — made clickable for touch. */
    for (int i = 0; i < APS_VIEW_ROWS; ++i) {
        s_ap_rows[i] = lv_label_create(s_aps_scr);
        lv_label_set_text(s_ap_rows[i], "");
        lv_obj_set_style_text_color(s_ap_rows[i], COL_CYAN, 0);
        lv_obj_align(s_ap_rows[i], LV_ALIGN_TOP_LEFT, 4, 20 + i * 22);

        /* Make the label tappable. Widen the hit area to full screen width. */
        lv_obj_add_flag(s_ap_rows[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_width(s_ap_rows[i], NESSO_LCD_WIDTH - 8);
        lv_obj_add_event_cb(s_ap_rows[i], ap_row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    /* Status bar at bottom */
    s_aps_status_label = lv_label_create(s_aps_scr);
    lv_label_set_text(s_aps_status_label, "KEY2:scroll KEY1:deauth");
    lv_obj_set_style_text_color(s_aps_status_label, COL_WHITE, 0);
    lv_obj_align(s_aps_status_label, LV_ALIGN_BOTTOM_MID, 0, -18);

    s_aps_view_label = lv_label_create(s_aps_scr);
    lv_label_set_text(s_aps_view_label, "[2/3] APS");
    lv_obj_set_style_text_color(s_aps_view_label, COL_WHITE, 0);
    lv_obj_align(s_aps_view_label, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/* Sort APs by RSSI descending (strongest first). Simple insertion sort. */
static void sort_aps_by_rssi(nesso_wardrive_ap_t *aps, size_t n)
{
    for (size_t i = 1; i < n; ++i) {
        nesso_wardrive_ap_t tmp = aps[i];
        size_t j = i;
        while (j > 0 && aps[j - 1].rssi_peak < tmp.rssi_peak) {
            aps[j] = aps[j - 1];
            --j;
        }
        aps[j] = tmp;
    }
}

static void refresh_aps_view(void)
{
    nesso_wardrive_ap_t all[16];
    size_t count = 0;
    nesso_wardrive_snapshot(all, 16, &count);
    sort_aps_by_rssi(all, count);

    s_ap_snap_count = count < APS_VIEW_ROWS ? count : APS_VIEW_ROWS;
    memcpy(s_ap_snap, all, s_ap_snap_count * sizeof(s_ap_snap[0]));

    if (s_aps_cursor >= (int)s_ap_snap_count) {
        s_aps_cursor = s_ap_snap_count > 0 ? (int)s_ap_snap_count - 1 : 0;
    }

    for (int i = 0; i < APS_VIEW_ROWS; ++i) {
        if ((size_t)i < s_ap_snap_count) {
            const nesso_wardrive_ap_t *ap = &s_ap_snap[i];

            /* Build display name: SSID if known, otherwise MAC suffix. */
            char name[16];
            if (ap->ssid[0]) {
                snprintf(name, sizeof(name), "%.10s", ap->ssid);
            } else {
                snprintf(name, sizeof(name), "%02X:%02X:%02X",
                         ap->bssid[3], ap->bssid[4], ap->bssid[5]);
            }

            char row[32];
            snprintf(row, sizeof(row), "%-10s %2u %3d",
                     name, ap->primary_channel, ap->rssi_peak);
            lv_label_set_text(s_ap_rows[i], row);

            /* Color coding:
             *   Selected + attacking = RED (blinking handled by timer)
             *   Selected = YELLOW
             *   Normal = CYAN */
            lv_color_t color;
            bool is_target = s_deauth_active &&
                memcmp(ap->bssid, s_deauth_bssid, 6) == 0;
            if (is_target) {
                color = COL_RED;
            } else if (i == s_aps_cursor) {
                color = COL_YELLOW;
            } else {
                color = COL_CYAN;
            }
            lv_obj_set_style_text_color(s_ap_rows[i], color, 0);
        } else {
            lv_label_set_text(s_ap_rows[i], "");
        }
    }

    /* Status bar */
    if (s_deauth_active) {
        char status[64];
        snprintf(status, sizeof(status), ">> DEAUTH x%lu ch%u",
                 (unsigned long)s_deauth_sent, s_deauth_channel);
        lv_label_set_text(s_aps_status_label, status);
        lv_obj_set_style_text_color(s_aps_status_label, COL_RED, 0);
    } else {
        lv_label_set_text(s_aps_status_label, "v:scroll  hold:attack");
        lv_obj_set_style_text_color(s_aps_status_label, COL_WHITE, 0);
    }
}

/* -------------------- LORA view -------------------- */

static void build_lora_screen(void)
{
    s_lora_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_lora_scr, COL_BLACK, 0);
    lv_obj_set_style_bg_opa  (s_lora_scr, LV_OPA_COVER, 0);

    lv_obj_t *t = lv_label_create(s_lora_scr);
    lv_label_set_text(t, "LORA\n(soon)");
    lv_obj_set_style_text_color(t, COL_MAGENTA, 0);
    lv_obj_center(t);

    lv_obj_t *v = lv_label_create(s_lora_scr);
    lv_label_set_text(v, "[3/3] LORA");
    lv_obj_set_style_text_color(v, COL_WHITE, 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/* -------------------- deauth attack -------------------- */

static void deauth_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_deauth_active) return;

    /* Send a burst of 10 deauth frames every 2 seconds. */
    esp_err_t err = nesso_wifi_send_deauth(s_deauth_bssid, NULL, 7, 10);
    if (err == ESP_OK) {
        s_deauth_sent += 10;
        /* Blink LED on each burst so there's physical feedback. */
        static bool led_state = false;
        led_state = !led_state;
        nesso_led(led_state);
    } else {
        ESP_LOGW(TAG, "deauth tx failed: %s", esp_err_to_name(err));
    }
}

static void start_deauth(int ap_index)
{
    if ((size_t)ap_index >= s_ap_snap_count) return;
    if (s_deauth_active) return;  /* already running */

    const nesso_wardrive_ap_t *ap = &s_ap_snap[ap_index];
    memcpy(s_deauth_bssid, ap->bssid, 6);
    s_deauth_channel = ap->primary_channel;
    memcpy(s_deauth_ssid, ap->ssid, sizeof(s_deauth_ssid));
    s_deauth_sent = 0;

    /* Lock channel so we stay on target's channel for PMKID capture. */
    nesso_wardrive_lock_channel(s_deauth_channel);

    /* Initial burst. */
    nesso_wifi_send_deauth(s_deauth_bssid, NULL, 7, 20);
    s_deauth_sent = 20;

    /* Repeating burst every 2 seconds via LVGL timer (runs inside the
     * LVGL task so we don't need a separate FreeRTOS task). */
    s_deauth_timer = lv_timer_create(deauth_timer_cb, 2000, NULL);
    s_deauth_active = true;

    ESP_LOGI(TAG, "DEAUTH started: %s ch=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x",
             s_deauth_ssid, s_deauth_channel,
             s_deauth_bssid[0], s_deauth_bssid[1], s_deauth_bssid[2],
             s_deauth_bssid[3], s_deauth_bssid[4], s_deauth_bssid[5]);
}

static void stop_deauth(void)
{
    if (!s_deauth_active) return;

    if (s_deauth_timer) {
        lv_timer_del(s_deauth_timer);
        s_deauth_timer = NULL;
    }
    s_deauth_active = false;
    nesso_led(false);  /* LED off when attack stops */

    /* Resume normal channel hopping. */
    nesso_wardrive_lock_channel(0);

    ESP_LOGI(TAG, "DEAUTH stopped: sent %lu frames to %s",
             (unsigned long)s_deauth_sent, s_deauth_ssid);
}

/* -------------------- live refresh timer -------------------- */

static void refresh_cb(lv_timer_t *t)
{
    (void)t;

    if (s_view == NESSO_UI_VIEW_DASH) {
        nesso_wardrive_status_t ws = {0};
        nesso_eapol_status_t es = {0};
        nesso_wardrive_status(&ws);
        nesso_eapol_status(&es);

        lv_label_set_text_fmt(s_chan_label,   "CH   %u",  ws.current_channel);
        lv_label_set_text_fmt(s_aps_label,    "APs  %u",  (unsigned)ws.total_aps);
        lv_label_set_text_fmt(s_beacon_label, "BCN  %lu", (unsigned long)ws.beacons_parsed);
        lv_label_set_text_fmt(s_pmkid_label,  "PMK  %lu", (unsigned long)es.pmkids_captured);
        lv_label_set_text_fmt(s_power_label,  "USB  %s",
                              nesso_usb_connected() ? "Y" : "N");

        /* Show deauth status prominently on dash when active. */
        if (s_deauth_active) {
            char atk[32];
            snprintf(atk, sizeof(atk), "ATK %.8s x%lu",
                     s_deauth_ssid[0] ? s_deauth_ssid : "???",
                     (unsigned long)s_deauth_sent);
            lv_label_set_text(s_dash_view_label, atk);
            /* Alternate red/magenta for attention. */
            static bool blink = false;
            blink = !blink;
            lv_obj_set_style_text_color(s_dash_view_label,
                                        blink ? COL_RED : COL_MAGENTA, 0);
        } else {
            lv_label_set_text(s_dash_view_label, "[1/3] DASH");
            lv_obj_set_style_text_color(s_dash_view_label, COL_WHITE, 0);
        }
    }

    if (s_view == NESSO_UI_VIEW_APS) {
        refresh_aps_view();
    }
}

/* -------------------- view switching -------------------- */

static lv_obj_t *screen_for(nesso_ui_view_t v)
{
    switch (v) {
    case NESSO_UI_VIEW_DASH: return s_dash_scr;
    case NESSO_UI_VIEW_APS:  return s_aps_scr;
    case NESSO_UI_VIEW_LORA: return s_lora_scr;
    default: return s_dash_scr;
    }
}

static void show_view_locked(nesso_ui_view_t v)
{
    if (v >= NESSO_UI_VIEW_COUNT) v = NESSO_UI_VIEW_DASH;
    s_view = v;
    lv_obj_t *scr = screen_for(v);
    if (scr) lv_scr_load(scr);

    /* Force an immediate refresh when entering a view. */
    if (v == NESSO_UI_VIEW_APS) {
        s_aps_cursor = 0;
        refresh_aps_view();
    }
}

esp_err_t nesso_ui_show(nesso_ui_view_t view)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    lvgl_port_lock(0);
    show_view_locked(view);
    lvgl_port_unlock();
    return ESP_OK;
}

/* -------------------- button handler task -------------------- */

static void button_task(void *arg)
{
    (void)arg;
    QueueHandle_t q = nesso_buttons_event_queue();
    if (!q) {
        ESP_LOGW(TAG, "no button queue");
        s_button_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    nesso_btn_event_t evt;
    while (1) {
        if (xQueueReceive(q, &evt, portMAX_DELAY) != pdTRUE) continue;

        /*
         * Button map:
         *   KEY1 short  = scroll down AP list (APS view) / jump to APS (other views)
         *   KEY2 short  = cycle views (DASH -> APS -> LORA -> DASH)
         *   KEY2 long   = on APS: start/stop deauth on highlighted AP
         *   KEY1 long   = stop deauth + go to DASH (emergency stop)
         */

        /* Emergency stop: KEY1 long-press from any view. */
        if (evt.key == NESSO_KEY1 && evt.type == NESSO_BTN_EVT_LONG_PRESS) {
            lvgl_port_lock(0);
            if (s_deauth_active) stop_deauth();
            lvgl_port_unlock();
            nesso_ui_show(NESSO_UI_VIEW_DASH);
            continue;
        }

        /* KEY2 long-press on APS = attack / stop. */
        if (evt.key == NESSO_KEY2 && evt.type == NESSO_BTN_EVT_LONG_PRESS) {
            if (s_view == NESSO_UI_VIEW_APS) {
                lvgl_port_lock(0);
                if (s_deauth_active) {
                    stop_deauth();
                } else {
                    start_deauth(s_aps_cursor);
                }
                lvgl_port_unlock();
            }
            continue;
        }

        if (evt.type != NESSO_BTN_EVT_PRESS) continue;

        if (evt.key == NESSO_KEY2) {
            /* KEY2 short = next view. */
            nesso_ui_view_t next = (nesso_ui_view_t)((s_view + 1) % NESSO_UI_VIEW_COUNT);
            nesso_ui_show(next);
        } else if (evt.key == NESSO_KEY1) {
            if (s_view == NESSO_UI_VIEW_APS) {
                /* KEY1 short on APS = scroll cursor down. */
                lvgl_port_lock(0);
                s_aps_cursor = (s_aps_cursor + 1) %
                    (s_ap_snap_count > 0 ? (int)s_ap_snap_count : 1);
                refresh_aps_view();
                lvgl_port_unlock();
            } else {
                /* KEY1 short on DASH/LORA = jump to APS. */
                nesso_ui_show(NESSO_UI_VIEW_APS);
            }
        }
    }
}

/* -------------------- init -------------------- */

esp_err_t nesso_ui_init(void)
{
    if (s_up) return ESP_OK;

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = nesso_lcd_panel_io(),
        .panel_handle  = nesso_lcd_panel(),
        .buffer_size   = NESSO_LCD_WIDTH * 40,
        .double_buffer = true,
        .hres          = NESSO_LCD_WIDTH,
        .vres          = NESSO_LCD_HEIGHT,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rotation      = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma   = true,
            .swap_bytes = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    /* 2a. Touch input — DISABLED for now. The FT6336 + shared GPIO3 INT
     * line causes spurious events. TODO: investigate INT pin conflict
     * with BMI270 IMU (both share GPIO3) or add debounce/validation.
     * Buttons work fine for all navigation + deauth. */
#if 0  /* Touch — re-enable after debugging GPIO3 conflict */
    esp_lcd_panel_io_i2c_config_t tp_io_cfg =
        ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_err_t tp_err = esp_lcd_new_panel_io_i2c(
        nesso_i2c_bus(), &tp_io_cfg, &tp_io);
    if (tp_err == ESP_OK) {
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = NESSO_LCD_WIDTH,
            .y_max = NESSO_LCD_HEIGHT,
            .rst_gpio_num = -1,
            .int_gpio_num = NESSO_GPIO_TOUCH_IMU_INT,
            .levels = { .reset = 0, .interrupt = 0 },
            .flags  = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        };
        tp_err = esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_touch);
        if (tp_err == ESP_OK && s_touch) {
            const lvgl_port_touch_cfg_t touch_cfg = {
                .disp = s_disp, .handle = s_touch,
            };
            s_touch_indev = lvgl_port_add_touch(&touch_cfg);
            ESP_LOGI(TAG, "touch input registered");
        }
    }
#endif

    lvgl_port_lock(0);
    build_dash_screen();
    build_aps_screen();
    build_lora_screen();
    show_view_locked(NESSO_UI_VIEW_DASH);
    s_refresh_timer = lv_timer_create(refresh_cb, 250, NULL);
    lvgl_port_unlock();

    BaseType_t ok = xTaskCreate(button_task, "nesso_ui_btn", 6144, NULL, 4, &s_button_task);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "ui button task failed to start (non-fatal)");
    }

    s_up = true;
    ESP_LOGI(TAG, "UI up (dashboard)");
    return ESP_OK;
}

esp_err_t nesso_ui_deinit(void)
{
    if (!s_up) return ESP_OK;
    if (s_deauth_active) { lvgl_port_lock(0); stop_deauth(); lvgl_port_unlock(); }
    if (s_button_task) { vTaskDelete(s_button_task); s_button_task = NULL; }
    lvgl_port_lock(0);
    if (s_refresh_timer) { lv_timer_del(s_refresh_timer); s_refresh_timer = NULL; }
    lvgl_port_unlock();
    if (s_disp) { lvgl_port_remove_disp(s_disp); s_disp = NULL; }
    s_up = false;
    return ESP_OK;
}

bool nesso_ui_is_up(void) { return s_up; }
