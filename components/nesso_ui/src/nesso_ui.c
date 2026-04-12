/*
 * nesso_ui.c — LVGL dashboard for DAVEY JONES.
 */

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "nesso_bsp.h"
#include "nesso_buttons.h"
#include "nesso_lcd.h"
#include "nesso_ui.h"

/* Best-effort pulls from the recon components. Their headers are cheap
 * and we want live numbers on screen. */
#if __has_include("nesso_wardrive.h")
#  include "nesso_wardrive.h"
#  define HAVE_WARDRIVE 1
#endif
#if __has_include("nesso_eapol.h")
#  include "nesso_eapol.h"
#  define HAVE_EAPOL 1
#endif

static const char *TAG = "nesso_ui";

/* -------------------- Palette D (RGB565-exact) -------------------- */

#define COL_BLACK   lv_color_hex(0x000000)
#define COL_CYAN    lv_color_hex(0x00FFFF)
#define COL_MAGENTA lv_color_hex(0xFF00FF)
#define COL_YELLOW  lv_color_hex(0xFFFF00)
#define COL_WHITE   lv_color_hex(0xFFFFFF)

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
static lv_obj_t *s_view_label      = NULL;

/* Placeholder screens for other views. */
static lv_obj_t *s_aps_scr  = NULL;
static lv_obj_t *s_lora_scr = NULL;

static lv_timer_t   *s_refresh_timer = NULL;
static TaskHandle_t  s_button_task   = NULL;

/* -------------------- widget helpers -------------------- */

static lv_obj_t *make_row_label(lv_obj_t *parent, int y, lv_color_t color,
                                 const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 6, y);
    return lbl;
}

/* -------------------- dashboard screen build -------------------- */

static void build_dash_screen(void)
{
    s_dash_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_dash_scr, COL_BLACK, 0);
    lv_obj_set_style_bg_opa  (s_dash_scr, LV_OPA_COVER, 0);

    /* Title bar */
    s_title_label = lv_label_create(s_dash_scr);
    lv_label_set_text(s_title_label, "DAVEY JONES");
    lv_obj_set_style_text_color(s_title_label, COL_MAGENTA, 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 4);

    /* Counter rows. Nesso is 135×240 portrait; 20 px of line spacing fits
     * six rows + title with a bit of breathing room. */
    int y = 28;
    s_chan_label   = make_row_label(s_dash_scr, y, COL_CYAN,  "CH   --");
    y += 22;
    s_aps_label    = make_row_label(s_dash_scr, y, COL_CYAN,  "APs  0");
    y += 22;
    s_beacon_label = make_row_label(s_dash_scr, y, COL_CYAN,  "BCN  0");
    y += 22;
    s_pmkid_label  = make_row_label(s_dash_scr, y, COL_YELLOW,"PMK  0");
    y += 22;
    s_power_label  = make_row_label(s_dash_scr, y, COL_CYAN,  "USB  ?");

    /* Bottom view indicator */
    s_view_label = lv_label_create(s_dash_scr);
    lv_obj_set_style_text_color(s_view_label, COL_WHITE, 0);
    lv_label_set_text(s_view_label, "[1/3] DASH");
    lv_obj_align(s_view_label, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_aps_screen(void)
{
    s_aps_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_aps_scr, COL_BLACK, 0);
    lv_obj_set_style_bg_opa  (s_aps_scr, LV_OPA_COVER, 0);

    lv_obj_t *t = lv_label_create(s_aps_scr);
    lv_label_set_text(t, "TOP APs\n(soon)");
    lv_obj_set_style_text_color(t, COL_CYAN, 0);
    lv_obj_center(t);

    lv_obj_t *v = lv_label_create(s_aps_scr);
    lv_label_set_text(v, "[2/3] APS");
    lv_obj_set_style_text_color(v, COL_WHITE, 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -4);
}

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

/* -------------------- live refresh timer -------------------- */

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (s_view != NESSO_UI_VIEW_DASH) return;

#ifdef HAVE_WARDRIVE
    nesso_wardrive_status_t ws;
    if (nesso_wardrive_status(&ws) == ESP_OK) {
        lv_label_set_text_fmt(s_chan_label,   "CH   %u",  ws.current_channel);
        lv_label_set_text_fmt(s_aps_label,    "APs  %u",  (unsigned)ws.total_aps);
        lv_label_set_text_fmt(s_beacon_label, "BCN  %lu", (unsigned long)ws.beacons_parsed);
    }
#else
    lv_label_set_text(s_chan_label,   "CH   --");
    lv_label_set_text(s_aps_label,    "APs  --");
    lv_label_set_text(s_beacon_label, "BCN  --");
#endif

#ifdef HAVE_EAPOL
    nesso_eapol_status_t es;
    if (nesso_eapol_status(&es) == ESP_OK) {
        lv_label_set_text_fmt(s_pmkid_label, "PMK  %lu",
                              (unsigned long)es.pmkids_captured);
    }
#else
    lv_label_set_text(s_pmkid_label, "PMK  --");
#endif

    lv_label_set_text_fmt(s_power_label, "USB  %s",
                          nesso_usb_connected() ? "Y" : "N");
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
        ESP_LOGW(TAG, "no button queue — UI view switching disabled");
        s_button_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    nesso_btn_event_t evt;
    while (1) {
        if (xQueueReceive(q, &evt, portMAX_DELAY) != pdTRUE) continue;
        if (evt.type != NESSO_BTN_EVT_PRESS) continue;

        if (evt.key == NESSO_KEY2) {
            /* KEY2 short-press = next view */
            nesso_ui_view_t next = (nesso_ui_view_t)((s_view + 1) % NESSO_UI_VIEW_COUNT);
            nesso_ui_show(next);
        }
        /* KEY1 reserved — forward-compat hook for "select/action" semantics. */
    }
}

/* -------------------- init -------------------- */

esp_err_t nesso_ui_init(void)
{
    if (s_up) return ESP_OK;

    /* 1. LVGL port — handles task, tick, buffer, flush, byte swap. */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = nesso_lcd_panel_io(),
        .panel_handle  = nesso_lcd_panel(),
        /* 40-row partial buffers. 135 × 40 × 2 = 10.8 KB per buffer,
         * double-buffered = ~22 KB. */
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
            .swap_bytes = true,   /* ST7789 wants big-endian 565 */
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    /* 2. Build the screens while holding the LVGL lock. */
    lvgl_port_lock(0);
    build_dash_screen();
    build_aps_screen();
    build_lora_screen();
    show_view_locked(NESSO_UI_VIEW_DASH);
    s_refresh_timer = lv_timer_create(refresh_cb, 250 /* ms */, NULL);
    lvgl_port_unlock();

    /* 3. Button task to drive view switching. */
    BaseType_t ok = xTaskCreate(button_task, "nesso_ui_btn", 4096, NULL, 4, &s_button_task);
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
    if (s_button_task) { vTaskDelete(s_button_task); s_button_task = NULL; }
    lvgl_port_lock(0);
    if (s_refresh_timer) { lv_timer_del(s_refresh_timer); s_refresh_timer = NULL; }
    lvgl_port_unlock();
    if (s_disp) {
        lvgl_port_remove_disp(s_disp);
        s_disp = NULL;
    }
    s_up = false;
    return ESP_OK;
}

bool nesso_ui_is_up(void) { return s_up; }
