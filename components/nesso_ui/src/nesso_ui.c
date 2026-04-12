/*
 * nesso_ui.c — DAVEY JONES full menu system.
 *
 * Screen hierarchy:
 *   SPLASH → (double-tap/any key) → MAIN MENU
 *     ├── WiFi
 *     │   ├── Scan (runs scan + shows results)
 *     │   ├── AP List (wardrive results, scrollable)
 *     │   ├── Deauth (select target → attack screen)
 *     │   └── Beacon Spam (broadcast fake APs)
 *     ├── Sub-GHz (placeholder)
 *     ├── IR (placeholder)
 *     └── Settings (placeholder)
 *
 * Controls:
 *   KEY1 = scroll down / select
 *   KEY2 = back / secondary action
 *   Double-tap screen = select (on menus) / start attack (on AP list)
 *   Long-press KEY1 = back to main menu (from anywhere)
 */

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
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
#include "nesso_ir.h"
#include "nesso_subghz.h"
#include "nesso_buzzer.h"

static const char *TAG = "nesso_ui";

/* -------------------- Palette -------------------- */

#define COL_BLACK   lv_color_hex(0x000000)
#define COL_CYAN    lv_color_hex(0x00FFFF)
#define COL_MAGENTA lv_color_hex(0xFF00FF)
#define COL_YELLOW  lv_color_hex(0xFFFF00)
#define COL_WHITE   lv_color_hex(0xFFFFFF)
#define COL_RED     lv_color_hex(0xFF0000)
#define COL_GREEN   lv_color_hex(0x00FF00)

/* -------------------- UI state machine -------------------- */

typedef enum {
    UI_SPLASH,
    UI_MAIN_MENU,
    UI_WIFI_MENU,
    UI_WIFI_SCANNING,
    UI_WIFI_AP_LIST,
    UI_WIFI_DEAUTH_SELECT,
    UI_WIFI_DEAUTH_ACTIVE,
    UI_WIFI_BEACON_SPAM,
    UI_WIFI_DAVEYGOTCHI,
    UI_SUBGHZ_MENU,
    UI_SUBGHZ_ANALYZER,
    UI_SUBGHZ_CAPTURE,
    UI_SUBGHZ_REPLAY,
    UI_IR_MENU,
    UI_IR_TVBGONE,
    UI_IR_SAMSUNG_REMOTE,
    UI_IR_VOLUME_MAX,
    UI_IR_CHANNEL_CHAOS,
} ui_state_t;

static bool              s_up            = false;
static lv_display_t     *s_disp          = NULL;
static ui_state_t        s_state         = UI_SPLASH;
static lv_timer_t       *s_refresh_timer = NULL;
static TaskHandle_t      s_button_task   = NULL;

/* Touch */
static esp_lcd_touch_handle_t s_touch = NULL;
static lv_indev_t            *s_touch_indev = NULL;

/* Current screen object — rebuilt on each state change. */
static lv_obj_t *s_screen = NULL;

/* Shared state for AP views. */
#define AP_VIEW_ROWS 8
static nesso_wardrive_ap_t s_ap_snap[16];
static size_t              s_ap_snap_count = 0;
static int                 s_cursor        = 0;

/* Deauth state */
static bool     s_deauth_active   = false;
static uint8_t  s_deauth_bssid[6] = {0};
static uint8_t  s_deauth_channel  = 0;
static char     s_deauth_ssid[33] = {0};
static uint32_t s_deauth_sent     = 0;
static lv_timer_t *s_deauth_timer = NULL;
static uint32_t s_atk_start_ms    = 0;

/* Screen-specific labels (set during build, refreshed by timer). */
static lv_obj_t *s_dyn_labels[12];
static int       s_dyn_count = 0;

/* Deferred navigation from touch callbacks. */
static ui_state_t s_pending_nav = UI_SPLASH;
static bool       s_has_pending = false;

/* Navigation direction — affects transition animation. */
static bool s_nav_back = false;

/* TV-B-Gone one-shot flag. */
static bool s_tvbg_done = false;

/* Beacon spam SSIDs. */
static const char *s_spam_list[] = {
    "FBI Surveillance Van",
    "NSA_Field_Office_4",
    "CIA Black Site WiFi",
    "DEA Task Force",
    "GCHQ Mobile Unit",
    "MI6 Undercover Net",
    "Interpol Stakeout",
    "Secret Service USSS",
    "Area 51 Guest WiFi",
    "Witness Protection",
    "Totally Not A Cop",
    "Free Candy Van WiFi",
};
#define SPAM_COUNT (sizeof(s_spam_list) / sizeof(s_spam_list[0]))

/* -------------------- helpers -------------------- */

static lv_obj_t *make_label(lv_obj_t *parent, int y, lv_color_t color,
                             const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 6, y);
    return lbl;
}

/* Track a label for dynamic refresh. */
static void track(lv_obj_t *lbl)
{
    if (s_dyn_count < 12) s_dyn_labels[s_dyn_count++] = lbl;
}

static void sort_aps_by_rssi(nesso_wardrive_ap_t *aps, size_t n)
{
    for (size_t i = 1; i < n; ++i) {
        nesso_wardrive_ap_t tmp = aps[i];
        size_t j = i;
        while (j > 0 && aps[j-1].rssi_peak < tmp.rssi_peak) {
            aps[j] = aps[j-1]; --j;
        }
        aps[j] = tmp;
    }
}

static void refresh_ap_snapshot(void)
{
    nesso_wardrive_snapshot(s_ap_snap, 16, &s_ap_snap_count);
    sort_aps_by_rssi(s_ap_snap, s_ap_snap_count);
    if (s_cursor >= (int)s_ap_snap_count)
        s_cursor = s_ap_snap_count > 0 ? (int)s_ap_snap_count - 1 : 0;
}

/* -------------------- deauth control -------------------- */

static void deauth_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_deauth_active) return;
    esp_err_t err = nesso_wifi_send_deauth(s_deauth_bssid, NULL, 7, 10);
    if (err == ESP_OK) {
        s_deauth_sent += 10;
        static bool led = false;
        led = !led;
        nesso_led(led);
    }
}

static void start_deauth(int idx)
{
    if ((size_t)idx >= s_ap_snap_count || s_deauth_active) return;
    const nesso_wardrive_ap_t *ap = &s_ap_snap[idx];
    memcpy(s_deauth_bssid, ap->bssid, 6);
    s_deauth_channel = ap->primary_channel;
    memcpy(s_deauth_ssid, ap->ssid, sizeof(s_deauth_ssid));
    s_deauth_sent = 0;
    s_atk_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    nesso_wardrive_lock_channel(s_deauth_channel);
    nesso_wifi_send_deauth(s_deauth_bssid, NULL, 7, 20);
    s_deauth_sent = 20;
    s_deauth_timer = lv_timer_create(deauth_timer_cb, 2000, NULL);
    s_deauth_active = true;
}

static void stop_deauth(void)
{
    if (!s_deauth_active) return;
    if (s_deauth_timer) { lv_timer_del(s_deauth_timer); s_deauth_timer = NULL; }
    s_deauth_active = false;
    nesso_led(false);
    nesso_wardrive_lock_channel(0);
}

/* -------------------- universal double-tap select -------------------- */

static uint32_t s_last_tap_ms = 0;
#define DOUBLE_TAP_MS 500

/* Menu item type — needed for forward declarations. */
typedef struct { const char *label; ui_state_t target; } menu_item_t;

static void handle_select(void);
static const menu_item_t *current_menu_items(int *out_count);

static void screen_tapped(lv_event_t *e)
{
    (void)e;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (s_last_tap_ms && (now - s_last_tap_ms) < DOUBLE_TAP_MS) {
        handle_select();
        s_last_tap_ms = 0;
    } else {
        s_last_tap_ms = now;
    }
}

/* -------------------- screen builders -------------------- */

static void navigate(ui_state_t state);

#include "splash_img.h"

/* Splash */
static void build_splash(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    /* Kraken image — 135×200 RGB565, fills most of the screen. */
    lv_obj_t *canvas = lv_canvas_create(s_screen);
    lv_canvas_set_buffer(canvas, (void *)splash_data, SPLASH_W, SPLASH_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_align(canvas, LV_ALIGN_TOP_MID, 0, 0);

    /* Title overlaid on the image — fades in after 500ms. */
    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "DAVEY JONES");
    lv_obj_set_style_text_color(title, COL_MAGENTA, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 205);
    lv_obj_set_style_opa(title, LV_OPA_TRANSP, 0);

    lv_anim_t a1;
    lv_anim_init(&a1);
    lv_anim_set_var(&a1, title);
    lv_anim_set_values(&a1, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a1, 800);
    lv_anim_set_delay(&a1, 500);
    lv_anim_set_exec_cb(&a1, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_start(&a1);

    lv_obj_t *sub = lv_label_create(s_screen);
    lv_label_set_text(sub, "v0.1 // tap to start");
    lv_obj_set_style_text_color(sub, COL_CYAN, 0);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_opa(sub, LV_OPA_TRANSP, 0);

    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, sub);
    lv_anim_set_values(&a2, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a2, 600);
    lv_anim_set_delay(&a2, 1200);
    lv_anim_set_exec_cb(&a2, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_start(&a2);
}

/* Menu item builder helper (typedef is above, near forward declarations). */

/* Styled title bar with accent line. */
static void make_title_bar(lv_obj_t *parent, const char *title)
{
    /* Title text. */
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, COL_MAGENTA, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);

    /* Accent line under title. */
    static lv_point_precise_t line_pts[] = {{0, 0}, {125, 0}};
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, line_pts, 2);
    lv_obj_set_style_line_color(line, COL_MAGENTA, 0);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_opa(line, LV_OPA_70, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 22);
}

static void build_menu(const char *title, const menu_item_t *items, int count)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    make_title_bar(s_screen, title);

    s_cursor = 0;
    s_dyn_count = 0;
    for (int i = 0; i < count && i < 10; ++i) {
        /* Each menu item is a container with padding for the highlight bg.
         * Must NOT be clickable — otherwise it intercepts touches meant
         * for the screen's double-tap handler. */
        lv_obj_t *row = lv_obj_create(s_screen);
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, 125, 24);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 28 + i * 26);

        /* Highlight background — only visible on selected item. */
        if (i == 0) {
            lv_obj_set_style_bg_color(row, COL_MAGENTA, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_30, 0);
            lv_obj_set_style_radius(row, 4, 0);
        }

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, items[i].label);
        lv_obj_set_style_text_color(lbl, i == 0 ? COL_YELLOW : COL_CYAN, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);

        track(row);  /* track the container, not the label */
    }

    /* Hint with subtle styling. */
    lv_obj_t *hint = lv_label_create(s_screen);
    lv_label_set_text(hint, "btn:scroll  2xtap:sel");
    lv_obj_set_style_text_color(hint, COL_WHITE, 0);
    lv_obj_set_style_opa(hint, LV_OPA_60, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/* Main Menu */
static const menu_item_t s_main_items[] = {
    { "> WiFi",    UI_WIFI_MENU },
    { "> Sub-GHz", UI_SUBGHZ_MENU },
    { "> IR",      UI_IR_MENU },
};
#define MAIN_ITEM_COUNT 3

/* Sub-GHz Menu */
static const menu_item_t s_subghz_items[] = {
    { "> Analyzer",  UI_SUBGHZ_ANALYZER },
    { "> Capture",   UI_SUBGHZ_CAPTURE },
    { "> Replay",    UI_SUBGHZ_REPLAY },
};
#define SUBGHZ_ITEM_COUNT 3

/* Sub-GHz state */
static subghz_band_t     s_subghz_band = SUBGHZ_BAND_WIDE;
static subghz_spectrum_t s_spectrum = {0};
static subghz_capture_t  s_capture  = {0};
static bool              s_has_capture = false;

/* IR Menu */
static const menu_item_t s_ir_items[] = {
    { "> TV-B-Gone",       UI_IR_TVBGONE },
    { "> Samsung Remote",  UI_IR_SAMSUNG_REMOTE },
    { "> Volume MAX",      UI_IR_VOLUME_MAX },
    { "> Channel Chaos",   UI_IR_CHANNEL_CHAOS },
};
#define IR_ITEM_COUNT 4

static void build_main_menu(void)
{
    build_menu("DAVEY JONES", s_main_items, MAIN_ITEM_COUNT);

    /* Show live stats at bottom. */
    nesso_wardrive_status_t ws = {0};
    nesso_eapol_status_t es = {0};
    nesso_wardrive_status(&ws);
    nesso_eapol_status(&es);
    lv_obj_t *stats = make_label(s_screen, 28 + MAIN_ITEM_COUNT * 22 + 10, COL_CYAN, "");
    lv_label_set_text_fmt(stats, "APs:%u PMK:%lu ch%u",
                          (unsigned)ws.total_aps,
                          (unsigned long)es.pmkids_captured,
                          ws.current_channel);
    track(stats);
}

/* WiFi Menu */
static const menu_item_t s_wifi_items[] = {
    { "> Daveygotchi",  UI_WIFI_DAVEYGOTCHI },
    { "> Scan",         UI_WIFI_SCANNING },
    { "> AP List",      UI_WIFI_AP_LIST },
    { "> Deauth",       UI_WIFI_DEAUTH_SELECT },
    { "> Beacon Spam",  UI_WIFI_BEACON_SPAM },
};
#define WIFI_ITEM_COUNT 5

/* WiFi AP List / Deauth Select (shared builder). */
static void build_ap_list(const char *title, bool deauth_mode)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    make_title_bar(s_screen, title);

    s_dyn_count = 0;
    s_cursor = 0;
    refresh_ap_snapshot();

    for (int i = 0; i < AP_VIEW_ROWS; ++i) {
        lv_obj_t *row = make_label(s_screen, 28 + i * 22, COL_CYAN, "");
        track(row);
    }

    lv_obj_t *status = make_label(s_screen, 28 + AP_VIEW_ROWS * 22 + 4, COL_WHITE,
                                   deauth_mode ? "2xtap: attack" : "");
    lv_obj_set_style_opa(status, LV_OPA_60, 0);
    track(status);
}

static void refresh_ap_rows(bool deauth_mode)
{
    refresh_ap_snapshot();
    size_t show = s_ap_snap_count < AP_VIEW_ROWS ? s_ap_snap_count : AP_VIEW_ROWS;

    for (int i = 0; i < AP_VIEW_ROWS && i < s_dyn_count; ++i) {
        if ((size_t)i < show) {
            const nesso_wardrive_ap_t *ap = &s_ap_snap[i];
            bool sel = (i == s_cursor);
            bool target = s_deauth_active && memcmp(ap->bssid, s_deauth_bssid, 6) == 0;

            char name[16];
            if (ap->ssid[0])
                snprintf(name, sizeof(name), "%.*s", sel ? 14 : 10, ap->ssid);
            else
                snprintf(name, sizeof(name), "%02X:%02X:%02X",
                         ap->bssid[3], ap->bssid[4], ap->bssid[5]);

            char row[36];
            if (sel)
                snprintf(row, sizeof(row), ">%-14s", name);
            else
                snprintf(row, sizeof(row), " %-4s %-9s %3d",
                         ap->auth[0] ? ap->auth : "?", name, ap->rssi_peak);

            lv_label_set_text(s_dyn_labels[i], row);
            lv_obj_set_style_text_color(s_dyn_labels[i],
                target ? COL_RED : sel ? COL_YELLOW : COL_CYAN, 0);
        } else {
            lv_label_set_text(s_dyn_labels[i], "");
        }
    }

    /* Update status label (last tracked = index AP_VIEW_ROWS). */
    if (s_dyn_count > AP_VIEW_ROWS) {
        lv_obj_t *status = s_dyn_labels[AP_VIEW_ROWS];
        if (s_cursor < (int)s_ap_snap_count) {
            const nesso_wardrive_ap_t *sel = &s_ap_snap[s_cursor];
            char info[36];
            snprintf(info, sizeof(info), "%s ch%u %ddBm",
                     sel->auth[0] ? sel->auth : "?",
                     sel->primary_channel, sel->rssi_peak);
            lv_label_set_text(status, info);
            lv_obj_set_style_text_color(status, COL_YELLOW, 0);
        }
    }
}

/* Deauth Active screen. */
static void build_deauth_active(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    s_dyn_count = 0;
    lv_obj_t *t = make_label(s_screen, 4, COL_RED, ">> DEAUTH <<");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);
    track(t);  /* 0: title */

    int y = 30;
    make_label(s_screen, y, COL_WHITE, "TARGET"); y += 18;
    lv_obj_t *ssid  = make_label(s_screen, y, COL_YELLOW, s_deauth_ssid[0] ? s_deauth_ssid : "???"); y += 18;
    track(ssid); /* 1 */

    char mac[20];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             s_deauth_bssid[0], s_deauth_bssid[1], s_deauth_bssid[2],
             s_deauth_bssid[3], s_deauth_bssid[4], s_deauth_bssid[5]);
    make_label(s_screen, y, COL_CYAN, mac); y += 18;

    char ch_info[20];
    snprintf(ch_info, sizeof(ch_info), "ch%u", s_deauth_channel);
    make_label(s_screen, y, COL_CYAN, ch_info); y += 24;

    make_label(s_screen, y, COL_WHITE, "ATTACK"); y += 18;
    lv_obj_t *frames = make_label(s_screen, y, COL_RED, "Frames: 0"); y += 18;
    track(frames); /* 2 */
    lv_obj_t *pmk = make_label(s_screen, y, COL_YELLOW, "PMKIDs: 0"); y += 18;
    track(pmk); /* 3 */
    lv_obj_t *tm = make_label(s_screen, y, COL_CYAN, "Time: 0:00"); y += 18;
    track(tm); /* 4 */

    lv_obj_t *hint = lv_label_create(s_screen);
    lv_label_set_text(hint, "KEY1:stop  KEY2:back");
    lv_obj_set_style_text_color(hint, COL_WHITE, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void refresh_deauth_active(void)
{
    if (s_dyn_count < 5) return;

    /* Blink title. */
    static bool blink = false; blink = !blink;
    lv_label_set_text(s_dyn_labels[0], blink ? ">> DEAUTH <<" : "   DEAUTH   ");
    lv_obj_set_style_text_color(s_dyn_labels[0], blink ? COL_RED : COL_MAGENTA, 0);

    lv_label_set_text_fmt(s_dyn_labels[2], "Frames: %lu", (unsigned long)s_deauth_sent);

    nesso_eapol_status_t es = {0};
    nesso_eapol_status(&es);
    lv_label_set_text_fmt(s_dyn_labels[3], "PMKIDs: %lu", (unsigned long)es.pmkids_captured);

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t elapsed = (now_ms - s_atk_start_ms) / 1000;
    lv_label_set_text_fmt(s_dyn_labels[4], "Time: %lu:%02lu",
                          (unsigned long)(elapsed / 60), (unsigned long)(elapsed % 60));

    if (!s_deauth_active) {
        lv_label_set_text(s_dyn_labels[0], "STOPPED");
        lv_obj_set_style_text_color(s_dyn_labels[0], COL_WHITE, 0);
    }
}

/* Beacon Spam screen. */
static void build_beacon_spam(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    s_dyn_count = 0;

    lv_obj_t *t = make_label(s_screen, 4, COL_MAGENTA, "BEACON SPAM");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *status = make_label(s_screen, 30, COL_GREEN, "BROADCASTING...");
    track(status); /* 0 */

    int y = 55;
    for (int i = 0; i < 6 && i < (int)SPAM_COUNT; ++i) {
        make_label(s_screen, y, COL_CYAN, s_spam_list[i]);
        y += 18;
    }
    make_label(s_screen, y, COL_CYAN, "...");

    lv_obj_t *hint = lv_label_create(s_screen);
    lv_label_set_text(hint, "KEY2:stop & back");
    lv_obj_set_style_text_color(hint, COL_WHITE, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/* -------------------- DAVEYGOTCHI -------------------- */

typedef enum {
    MOOD_LURKING,    /* scanning, no target */
    MOOD_HUNTING,    /* locked onto target, deauthing */
    MOOD_FEASTING,   /* just captured a PMKID */
    MOOD_BORED,      /* nothing happening */
    MOOD_SLEEPING,   /* idle for a long time */
} davey_mood_t;

static davey_mood_t  s_davey_mood        = MOOD_LURKING;
static uint32_t      s_davey_start_ms    = 0;
static uint32_t      s_davey_last_pmk    = 0;   /* pmkid count at last check */
static uint32_t      s_davey_feast_until = 0;   /* timestamp to stop celebrating */
static uint32_t      s_davey_hunt_until  = 0;   /* timestamp to stop hunting current target */
static uint32_t      s_davey_next_hunt   = 0;   /* timestamp for next auto-target */
static int           s_davey_target_idx  = -1;

static const char *davey_face(davey_mood_t m)
{
    static bool blink = false;
    blink = !blink;  /* alternates each refresh (250ms) */

    switch (m) {
    case MOOD_LURKING:
        return blink ?
            "    /\\_/\\\n"
            "   ( o.o )\n"
            "   />   <\\\n"
            "  /|     |\\"
            :
            "    /\\_/\\\n"
            "   ( -.- )\n"  /* blink */
            "   />   <\\\n"
            "  /|     |\\";
    case MOOD_HUNTING:
        return
            "    /\\_/\\\n"
            "   ( >.< )\n"
            "   />|||<\\\n"
            "  /|  |  |\\";
    case MOOD_FEASTING:
        return blink ?
            "    /\\_/\\\n"
            "   ( ^.^ )\n"
            "   />   <\\\n"
            "  /| ~~~ |\\"
            :
            "    /\\_/\\\n"
            "   ( ^o^ )\n"
            "   />   <\\\n"
            "  /| ~~~ |\\";
    case MOOD_BORED:
        return
            "    /\\_/\\\n"
            "   ( -.- )\n"
            "   />   <\\\n"
            "   |     |";
    case MOOD_SLEEPING:
        return
            "    /\\_/\\\n"
            "   ( u.u )\n"
            "    >   <\n"
            "    | z |";
    }
    return "   ( ?.? )";
}

static const char *davey_text(davey_mood_t m)
{
    /* Multiple texts per mood for variety. */
    static uint8_t variant = 0;
    variant++;

    switch (m) {
    case MOOD_LURKING:
        return (variant & 3) == 0 ? "Lurking in the deep..."
             : (variant & 3) == 1 ? "Scanning the abyss..."
             : (variant & 3) == 2 ? "Searching for prey..."
             :                      "Eyes on the horizon...";
    case MOOD_HUNTING:
        return (variant & 1) ? "FOUND PREY!" : "Attacking...";
    case MOOD_FEASTING:
        return (variant & 1) ? "FEAST! Got a key!" : "Treasure acquired!";
    case MOOD_BORED:
        return (variant & 1) ? "Nothing out here..." : "The sea is quiet...";
    case MOOD_SLEEPING:
        return "zzZZzzZZzz...";
    }
    return "...";
}

static void build_daveygotchi(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    s_dyn_count = 0;

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "DAVEYGOTCHI");
    lv_obj_set_style_text_color(title, COL_MAGENTA, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    /* Face — big and centered. */
    lv_obj_t *face = lv_label_create(s_screen);
    lv_label_set_text(face, davey_face(MOOD_LURKING));
    lv_obj_set_style_text_color(face, COL_CYAN, 0);
    lv_obj_align(face, LV_ALIGN_TOP_MID, 0, 28);
    track(face); /* 0: face */

    /* Mood text. */
    lv_obj_t *mood = lv_label_create(s_screen);
    lv_label_set_text(mood, davey_text(MOOD_LURKING));
    lv_obj_set_style_text_color(mood, COL_YELLOW, 0);
    lv_obj_set_style_text_align(mood, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mood, LV_ALIGN_TOP_MID, 0, 75);
    track(mood); /* 1: mood text */

    /* Stats. */
    int y = 100;
    lv_obj_t *aps = make_label(s_screen, y, COL_CYAN, "APs: 0  PMK: 0"); y += 18;
    track(aps); /* 2 */
    lv_obj_t *tgt = make_label(s_screen, y, COL_CYAN, "Target: ---"); y += 18;
    track(tgt); /* 3 */
    lv_obj_t *ch = make_label(s_screen, y, COL_CYAN, "CH: --  BCN: 0"); y += 18;
    track(ch); /* 4 */
    lv_obj_t *up = make_label(s_screen, y, COL_CYAN, "Uptime: 0:00:00"); y += 18;
    track(up); /* 5 */

    lv_obj_t *hint = lv_label_create(s_screen);
    lv_label_set_text(hint, "autonomous mode");
    lv_obj_set_style_text_color(hint, COL_WHITE, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* Init autonomous state. */
    s_davey_mood = MOOD_LURKING;
    s_davey_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_davey_next_hunt = s_davey_start_ms + 5000;  /* first hunt after 5s */
    s_davey_target_idx = -1;

    nesso_eapol_status_t es = {0};
    nesso_eapol_status(&es);
    s_davey_last_pmk = es.pmkids_captured;
}

static void refresh_daveygotchi(void)
{
    if (s_dyn_count < 6) return;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

    nesso_wardrive_status_t ws = {0};
    nesso_eapol_status_t es = {0};
    nesso_wardrive_status(&ws);
    nesso_eapol_status(&es);

    /* Check if we got a new PMKID! */
    if (es.pmkids_captured > s_davey_last_pmk) {
        s_davey_mood = MOOD_FEASTING;
        s_davey_feast_until = now + 8000;  /* celebrate for 8 seconds */
        s_davey_last_pmk = es.pmkids_captured;
        /* Stop current deauth — we got what we wanted. */
        if (s_deauth_active) stop_deauth();
        nesso_led(true);
    }

    /* State machine for autonomous hunting. */
    if (s_davey_mood == MOOD_FEASTING && now > s_davey_feast_until) {
        s_davey_mood = MOOD_LURKING;
        nesso_led(false);
    }

    if (s_davey_mood == MOOD_HUNTING && now > s_davey_hunt_until) {
        /* Hunting timeout — didn't get a PMKID. Move on. */
        if (s_deauth_active) stop_deauth();
        s_davey_mood = MOOD_BORED;
        s_davey_next_hunt = now + 5000;  /* try another in 5s */
    }

    if (s_davey_mood == MOOD_BORED && now > s_davey_next_hunt) {
        s_davey_mood = MOOD_LURKING;
    }

    /* Auto-target: pick strongest AP we haven't captured a PMKID from. */
    if ((s_davey_mood == MOOD_LURKING) && now > s_davey_next_hunt && !s_deauth_active) {
        refresh_ap_snapshot();
        if (s_ap_snap_count > 0) {
            /* Pick a target — cycle through APs. */
            s_davey_target_idx = (s_davey_target_idx + 1) % (int)s_ap_snap_count;
            start_deauth(s_davey_target_idx);
            s_davey_mood = MOOD_HUNTING;
            s_davey_hunt_until = now + 15000;  /* hunt for 15 seconds */
            s_davey_next_hunt = now + 30000;   /* next hunt in 30s */
        }
    }

    if (ws.total_aps == 0 && (now - s_davey_start_ms) > 30000) {
        s_davey_mood = MOOD_SLEEPING;
    }

    /* Update display. */
    lv_label_set_text(s_dyn_labels[0], davey_face(s_davey_mood));

    /* Mood color varies. */
    lv_color_t face_color;
    switch (s_davey_mood) {
    case MOOD_FEASTING: face_color = COL_GREEN; break;
    case MOOD_HUNTING:  face_color = COL_RED; break;
    case MOOD_BORED:    face_color = COL_WHITE; break;
    default:            face_color = COL_CYAN; break;
    }
    lv_obj_set_style_text_color(s_dyn_labels[0], face_color, 0);

    lv_label_set_text(s_dyn_labels[1], davey_text(s_davey_mood));

    lv_label_set_text_fmt(s_dyn_labels[2], "APs: %u  PMK: %lu",
                          (unsigned)ws.total_aps,
                          (unsigned long)es.pmkids_captured);

    if (s_deauth_active) {
        char tgt[28];
        snprintf(tgt, sizeof(tgt), ">> %.12s",
                 s_deauth_ssid[0] ? s_deauth_ssid : "???");
        lv_label_set_text(s_dyn_labels[3], tgt);
        lv_obj_set_style_text_color(s_dyn_labels[3], COL_RED, 0);
    } else {
        lv_label_set_text(s_dyn_labels[3], "Target: scanning...");
        lv_obj_set_style_text_color(s_dyn_labels[3], COL_CYAN, 0);
    }

    lv_label_set_text_fmt(s_dyn_labels[4], "CH: %u  BCN: %lu",
                          ws.current_channel,
                          (unsigned long)ws.beacons_parsed);

    uint32_t uptime = (now - s_davey_start_ms) / 1000;
    lv_label_set_text_fmt(s_dyn_labels[5], "Up: %lu:%02lu:%02lu",
                          (unsigned long)(uptime / 3600),
                          (unsigned long)((uptime % 3600) / 60),
                          (unsigned long)(uptime % 60));
}

/* Placeholder screen. */
static void build_placeholder(const char *title)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    lv_obj_t *t = lv_label_create(s_screen);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, COL_MAGENTA, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *msg = lv_label_create(s_screen);
    lv_label_set_text(msg, "Coming soon");
    lv_obj_set_style_text_color(msg, COL_CYAN, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *hint = lv_label_create(s_screen);
    lv_label_set_text(hint, "KEY2:back");
    lv_obj_set_style_text_color(hint, COL_WHITE, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/* WiFi Scanning screen. */
static void build_scanning(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    lv_obj_t *msg = lv_label_create(s_screen);
    lv_label_set_text(msg, "Scanning...");
    lv_obj_set_style_text_color(msg, COL_YELLOW, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
}

/* -------------------- navigation -------------------- */

static void navigate(ui_state_t state)
{
    s_tvbg_done = false;

    /* Reset landscape rotation if leaving analyzer. */
    if (s_state == UI_SUBGHZ_ANALYZER && state != UI_SUBGHZ_ANALYZER) {
        lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_0);
    }

    s_state = state;
    s_dyn_count = 0;

    switch (state) {
    case UI_SPLASH:           build_splash(); break;
    case UI_MAIN_MENU:        build_main_menu(); break;
    case UI_WIFI_MENU:        build_menu("WiFi", s_wifi_items, WIFI_ITEM_COUNT); break;
    case UI_WIFI_SCANNING:    build_scanning(); break;
    case UI_WIFI_AP_LIST:     build_ap_list("AP LIST", false); break;
    case UI_WIFI_DEAUTH_SELECT: build_ap_list("SELECT TARGET", true); break;
    case UI_WIFI_DEAUTH_ACTIVE: build_deauth_active(); break;
    case UI_WIFI_BEACON_SPAM:
        nesso_wifi_beacon_spam_start(s_spam_list, SPAM_COUNT, 0);
        build_beacon_spam();
        break;
    case UI_WIFI_DAVEYGOTCHI: build_daveygotchi(); break;
    case UI_IR_SAMSUNG_REMOTE:
    {
        s_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        s_dyn_count = 0;
        s_cursor = 0;

        lv_obj_t *t = lv_label_create(s_screen);
        lv_label_set_text(t, "SAMSUNG REMOTE");
        lv_obj_set_style_text_color(t, COL_MAGENTA, 0);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 2);

        /* Remote buttons — scroll with KEY1, double-tap to send. */
        static const char *btn_names[] = {
            "Power", "Vol +", "Vol -", "Ch +", "Ch -",
            "Mute", "Source", "Menu", "Home", "Return",
        };
        for (int i = 0; i < 10; ++i) {
            lv_obj_t *row = make_label(s_screen, 22 + i * 20,
                                        i == 0 ? COL_YELLOW : COL_CYAN,
                                        btn_names[i]);
            track(row);
        }

        lv_obj_t *hint = lv_label_create(s_screen);
        lv_label_set_text(hint, "scroll + 2xtap:send");
        lv_obj_set_style_text_color(hint, COL_WHITE, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
        break;
    }
    case UI_IR_VOLUME_MAX:
    case UI_IR_CHANNEL_CHAOS:
    {
        const char *title = (state == UI_IR_VOLUME_MAX) ? "VOLUME MAX" : "CHANNEL CHAOS";
        const char *desc  = (state == UI_IR_VOLUME_MAX)
            ? "Spamming Vol+ to\nevery TV in range..."
            : "Rapid channel surf\non every TV...";
        s_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        s_dyn_count = 0;
        lv_obj_t *t = lv_label_create(s_screen);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, state == UI_IR_VOLUME_MAX ? COL_RED : COL_MAGENTA, 0);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_t *msg = make_label(s_screen, 35, COL_YELLOW, desc);
        (void)msg;
        lv_obj_t *cnt = make_label(s_screen, 80, COL_CYAN, "0 sent");
        track(cnt);
        lv_obj_t *hint = lv_label_create(s_screen);
        lv_label_set_text(hint, "KEY2:stop");
        lv_obj_set_style_text_color(hint, COL_WHITE, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
        break;
    }
    case UI_SUBGHZ_MENU:
        build_menu("Sub-GHz", s_subghz_items, SUBGHZ_ITEM_COUNT);
        break;
    case UI_SUBGHZ_ANALYZER:
    {
        /* Switch to landscape for wider waveform. */
        lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_90);

        s_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        s_dyn_count = 0;

        /* Landscape: 240 wide x 135 tall.
         * Push content down 10px to avoid rotation clipping at top. */
        lv_obj_t *band_lbl = make_label(s_screen, 10, COL_GREEN, "WIDE 400-930  2xtap:band");
        track(band_lbl); /* 0: band info */

        /* Canvas for waveform — 230 wide x 85 tall. */
        static lv_color_t cbuf[230 * 85];
        lv_obj_t *canvas = lv_canvas_create(s_screen);
        lv_canvas_set_buffer(canvas, cbuf, 230, 85, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 5, 28);
        lv_canvas_fill_bg(canvas, COL_BLACK, LV_OPA_COVER);
        track(canvas); /* 1: canvas */

        /* Peak info at bottom. */
        lv_obj_t *peak_lbl = make_label(s_screen, 118, COL_YELLOW, "Peak: ---");
        track(peak_lbl); /* 2: peak info */

        nesso_buzzer_init();
        break;
    }
    case UI_SUBGHZ_CAPTURE:
    {
        s_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        s_dyn_count = 0;

        make_label(s_screen, 4, COL_MAGENTA, "CAPTURE");
        lv_obj_t *st = make_label(s_screen, 30, COL_YELLOW, "Listening at 433.92 MHz...");
        track(st); /* 0 */
        lv_obj_t *cnt = make_label(s_screen, 55, COL_CYAN, "0 bytes");
        track(cnt); /* 1 */
        lv_obj_t *hint = lv_label_create(s_screen);
        lv_label_set_text(hint, "KEY2:stop + save");
        lv_obj_set_style_text_color(hint, COL_WHITE, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
        break;
    }
    case UI_SUBGHZ_REPLAY:
    {
        s_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        s_dyn_count = 0;

        make_label(s_screen, 4, COL_MAGENTA, "REPLAY");
        if (s_has_capture) {
            char info[40];
            snprintf(info, sizeof(info), "%lu Hz, %zu bytes",
                     (unsigned long)s_capture.freq_hz, s_capture.length);
            make_label(s_screen, 30, COL_CYAN, info);
            lv_obj_t *st = make_label(s_screen, 55, COL_YELLOW, "2xtap to transmit");
            track(st); /* 0 */
        } else {
            make_label(s_screen, 30, COL_RED, "No capture loaded");
            make_label(s_screen, 55, COL_WHITE, "Capture a signal first");
        }
        lv_obj_t *hint = lv_label_create(s_screen);
        lv_label_set_text(hint, "KEY2:back");
        lv_obj_set_style_text_color(hint, COL_WHITE, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
        break;
    }
    case UI_IR_MENU:          build_menu("Infrared", s_ir_items, IR_ITEM_COUNT); break;
    case UI_IR_TVBGONE:
    {
        s_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        s_dyn_count = 0;
        lv_obj_t *t = lv_label_create(s_screen);
        lv_label_set_text(t, "TV-B-GONE");
        lv_obj_set_style_text_color(t, COL_RED, 0);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_t *msg = make_label(s_screen, 40, COL_YELLOW, "Sending power codes...");
        track(msg);
        lv_obj_t *cnt = make_label(s_screen, 65, COL_CYAN, "0 / 20");
        track(cnt);
        make_label(s_screen, 100, COL_WHITE, "Point at TV and wait");
        lv_obj_t *hint = lv_label_create(s_screen);
        lv_label_set_text(hint, "KEY2:cancel");
        lv_obj_set_style_text_color(hint, COL_WHITE, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
        break;
    }
    }

    /* Every screen gets the universal double-tap handler. */
    if (s_screen) {
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_screen, screen_tapped, LV_EVENT_CLICKED, NULL);

        /* Smooth animated transitions. Back = slide right, forward = slide left. */
        lv_scr_load_anim_t anim;
        uint32_t dur = 200;

        if (state == UI_SPLASH) {
            anim = LV_SCR_LOAD_ANIM_FADE_IN;
            dur = 400;
        } else if (state == UI_MAIN_MENU && s_nav_back) {
            anim = LV_SCR_LOAD_ANIM_MOVE_RIGHT;
            dur = 250;
        } else if (state == UI_MAIN_MENU) {
            anim = LV_SCR_LOAD_ANIM_FADE_IN;
            dur = 300;
        } else if (s_nav_back) {
            anim = LV_SCR_LOAD_ANIM_MOVE_RIGHT;
        } else if (state == UI_WIFI_DEAUTH_ACTIVE || state == UI_WIFI_DAVEYGOTCHI ||
                   state == UI_SUBGHZ_ANALYZER || state == UI_WIFI_BEACON_SPAM) {
            anim = LV_SCR_LOAD_ANIM_OVER_TOP;
        } else {
            anim = LV_SCR_LOAD_ANIM_MOVE_LEFT;
        }

        s_nav_back = false;
        lv_scr_load_anim(s_screen, anim, dur, 0, true);
    }
}

/*
 * Universal "select" action — triggered by double-tap on any screen.
 * Does the right thing based on current state.
 */
static void handle_select(void)
{
    int count = 0;
    const menu_item_t *items = current_menu_items(&count);

    switch (s_state) {
    case UI_SPLASH:
        s_has_pending = true;
        s_pending_nav = UI_MAIN_MENU;
        break;

    case UI_MAIN_MENU:
    case UI_WIFI_MENU:
    case UI_IR_MENU:
    case UI_SUBGHZ_MENU:
        if (items && s_cursor < count) {
            s_has_pending = true;
            s_pending_nav = items[s_cursor].target;
        }
        break;

    case UI_WIFI_DEAUTH_SELECT:
        if (s_ap_snap_count > 0 && !s_deauth_active) {
            start_deauth(s_cursor);
            s_has_pending = true;
            s_pending_nav = UI_WIFI_DEAUTH_ACTIVE;
        }
        break;

    case UI_WIFI_DEAUTH_ACTIVE:
        stop_deauth();
        s_has_pending = true;
        s_pending_nav = UI_WIFI_DEAUTH_SELECT;
        break;

    case UI_WIFI_BEACON_SPAM:
        nesso_wifi_beacon_spam_stop();
        s_has_pending = true;
        s_pending_nav = UI_WIFI_MENU;
        break;

    case UI_WIFI_DAVEYGOTCHI:
        /* Double-tap on daveygotchi = exit. */
        if (s_deauth_active) stop_deauth();
        s_has_pending = true;
        s_pending_nav = UI_WIFI_MENU;
        break;

    case UI_IR_SAMSUNG_REMOTE:
    {
        /* Samsung TV IR codes (address 0x0707). */
        static const uint8_t sam_cmds[] = {
            0x02,  /* Power */
            0x07,  /* Vol + */
            0x0B,  /* Vol - */
            0x12,  /* Ch + */
            0x10,  /* Ch - */
            0x0F,  /* Mute */
            0x01,  /* Source */
            0x1A,  /* Menu */
            0x79,  /* Home / Smart Hub */
            0x58,  /* Return */
        };
        if (s_cursor >= 0 && s_cursor < 10) {
            if (!nesso_ir_is_ready()) nesso_ir_init();
            nesso_ir_send_samsung(0x0707, sam_cmds[s_cursor]);
            /* Brief LED flash as feedback. */
            nesso_led(true);
            vTaskDelay(pdMS_TO_TICKS(50));
            nesso_led(false);
        }
        break;
    }

    case UI_SUBGHZ_REPLAY:
        if (s_has_capture) {
            nesso_subghz_replay(&s_capture);
        }
        break;

    case UI_SUBGHZ_ANALYZER:
        /* Double-tap cycles bands. */
        s_subghz_band = (subghz_band_t)((s_subghz_band + 1) % SUBGHZ_BAND_COUNT);
        break;

    default:
        break;
    }
}

/* -------------------- menu navigation logic -------------------- */

/* Get the menu item array and count for the current state. */
static const menu_item_t *current_menu_items(int *out_count)
{
    switch (s_state) {
    case UI_MAIN_MENU: *out_count = MAIN_ITEM_COUNT; return s_main_items;
    case UI_WIFI_MENU: *out_count = WIFI_ITEM_COUNT; return s_wifi_items;
    case UI_IR_MENU:     *out_count = IR_ITEM_COUNT;     return s_ir_items;
    case UI_SUBGHZ_MENU: *out_count = SUBGHZ_ITEM_COUNT; return s_subghz_items;
    default: *out_count = 0; return NULL;
    }
}

/* Update cursor highlight in a menu. */
static void update_menu_cursor(int item_count)
{
    for (int i = 0; i < item_count && i < s_dyn_count; ++i) {
        bool sel = (i == s_cursor);
        lv_obj_t *row = s_dyn_labels[i];

        /* Highlight background on selected item. */
        if (sel) {
            lv_obj_set_style_bg_color(row, COL_MAGENTA, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_30, 0);
            lv_obj_set_style_radius(row, 4, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }

        /* Update text color on the label child. */
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, sel ? COL_YELLOW : COL_CYAN, 0);
        }
    }
}

/* -------------------- refresh timer -------------------- */

static void refresh_cb(lv_timer_t *t)
{
    (void)t;

    /* Apply deferred navigation from touch. */
    if (s_has_pending) {
        s_has_pending = false;
        navigate(s_pending_nav);
        return;
    }

    /* WiFi scanning auto-transition: run scan then show results. */
    if (s_state == UI_WIFI_SCANNING) {
        /* Scan is blocking — run it, then navigate to AP list. */
        nesso_wifi_scan(NULL, 0, NULL);
        navigate(UI_WIFI_AP_LIST);
        return;
    }

    /* Per-state refresh. */
    switch (s_state) {
    case UI_WIFI_AP_LIST:
    case UI_WIFI_DEAUTH_SELECT:
        refresh_ap_rows(s_state == UI_WIFI_DEAUTH_SELECT);
        break;
    case UI_WIFI_DEAUTH_ACTIVE:
        refresh_deauth_active();
        break;
    case UI_WIFI_DAVEYGOTCHI:
        refresh_daveygotchi();
        break;
    case UI_IR_TVBGONE:
    {
        /* Fire continuously — one full cycle per refresh tick. */
        static uint32_t s_tvbg_rounds = 0;
        if (!s_tvbg_done) {
            s_tvbg_done = true;  /* init flag — means "started" not "finished" */
            s_tvbg_rounds = 0;
            if (!nesso_ir_is_ready()) nesso_ir_init();
        }
        nesso_ir_tvbgone();
        s_tvbg_rounds++;
        if (s_dyn_count >= 2) {
            lv_label_set_text_fmt(s_dyn_labels[0], "Round %lu firing...",
                                  (unsigned long)s_tvbg_rounds);
            lv_label_set_text_fmt(s_dyn_labels[1], "%lu codes sent",
                                  (unsigned long)(s_tvbg_rounds * 20));
            static bool blink = false; blink = !blink;
            lv_obj_set_style_text_color(s_dyn_labels[0],
                                        blink ? COL_RED : COL_YELLOW, 0);
        }
        nesso_led(s_tvbg_rounds % 2 == 0);
        break;
    }
    case UI_SUBGHZ_ANALYZER:
    {
        if (s_dyn_count < 3) break;
        nesso_subghz_sweep(s_subghz_band, &s_spectrum);

        static const char *band_names[] = { "WIDE 400-930", "433 MHz", "868 MHz", "915 MHz" };
        lv_label_set_text_fmt(s_dyn_labels[0], "%s  2xtap:band",
                              band_names[s_subghz_band % SUBGHZ_BAND_COUNT]);

        lv_label_set_text_fmt(s_dyn_labels[2], "Peak: %lu.%02lu MHz %ddBm",
                              (unsigned long)(s_spectrum.peak_freq_hz / 1000000),
                              (unsigned long)((s_spectrum.peak_freq_hz % 1000000) / 10000),
                              s_spectrum.rssi_peak);

        /* Draw waveform on canvas (230 x 85). */
        lv_obj_t *canvas = s_dyn_labels[1];
        lv_canvas_fill_bg(canvas, COL_BLACK, LV_OPA_COVER);

        for (int x = 0; x < SUBGHZ_SPECTRUM_POINTS && x < 230; ++x) {
            int rssi = s_spectrum.rssi[x];
            int height = (rssi + 128) * 84 / 128;
            if (height < 0) height = 0;
            if (height > 84) height = 84;

            lv_color_t color;
            if (rssi > -60) color = COL_RED;
            else if (rssi > -80) color = COL_YELLOW;
            else if (rssi > -100) color = COL_GREEN;
            else color = COL_CYAN;

            for (int y = 84; y >= 84 - height; --y) {
                lv_canvas_set_px(canvas, x, y, color, LV_OPA_COVER);
            }
        }

        /* Flipper-style audio: continuous tone whose pitch tracks the
         * peak signal strength. Silence when nothing above noise floor.
         * Chirps rapidly when signal is strong. */
        if (s_spectrum.rssi_peak > -100) {
            /* Map -100..-30 dBm to 500..4000 Hz. */
            int strength = s_spectrum.rssi_peak + 100;  /* 0..70 */
            if (strength < 0) strength = 0;
            if (strength > 70) strength = 70;
            uint32_t pitch = 500 + (uint32_t)strength * 50;
            nesso_buzzer_tone(pitch, 0);  /* continuous — updated each sweep */
        } else {
            nesso_buzzer_off();
        }
        break;
    }
    case UI_SUBGHZ_CAPTURE:
    {
        static bool s_cap_done = false;
        if (!s_cap_done) {
            s_cap_done = true;
            esp_err_t err = nesso_subghz_capture(433920000, 5000, -70, &s_capture);
            if (err == ESP_OK) {
                s_has_capture = true;
                if (s_dyn_count >= 2) {
                    lv_label_set_text(s_dyn_labels[0], "Signal captured!");
                    lv_label_set_text_fmt(s_dyn_labels[1], "%zu bytes", s_capture.length);
                    lv_obj_set_style_text_color(s_dyn_labels[0], COL_GREEN, 0);
                }
                nesso_subghz_save(&s_capture, "/storage/capture.bin");
                nesso_buzzer_tone(3000, 200);
            } else {
                if (s_dyn_count >= 2) {
                    lv_label_set_text(s_dyn_labels[0], "No signal detected");
                    lv_obj_set_style_text_color(s_dyn_labels[0], COL_RED, 0);
                }
            }
        }
        break;
    }
    case UI_IR_VOLUME_MAX:
    case UI_IR_CHANNEL_CHAOS:
    {
        if (!nesso_ir_is_ready()) nesso_ir_init();
        static uint32_t s_ir_spam_cnt = 0;
        if (!s_tvbg_done) { s_tvbg_done = true; s_ir_spam_cnt = 0; }
        if (s_state == UI_IR_VOLUME_MAX) {
            nesso_ir_send_samsung(0x0707, 0x07);
            nesso_ir_send_nec(0x04, 0x02);
            nesso_ir_send_nec(0x01, 0x12);
        } else {
            if (s_ir_spam_cnt % 2 == 0) {
                nesso_ir_send_samsung(0x0707, 0x12);
                nesso_ir_send_nec(0x04, 0x00);
            } else {
                nesso_ir_send_samsung(0x0707, 0x10);
                nesso_ir_send_nec(0x04, 0x01);
            }
        }
        s_ir_spam_cnt++;
        if (s_dyn_count >= 1)
            lv_label_set_text_fmt(s_dyn_labels[0], "%lu sent", (unsigned long)s_ir_spam_cnt);
        nesso_led(s_ir_spam_cnt % 2 == 0);
        break;
    }
    case UI_MAIN_MENU:
        /* Refresh stats line. */
        if (s_dyn_count > MAIN_ITEM_COUNT) {
            nesso_wardrive_status_t ws = {0};
            nesso_eapol_status_t es = {0};
            nesso_wardrive_status(&ws);
            nesso_eapol_status(&es);
            lv_label_set_text_fmt(s_dyn_labels[MAIN_ITEM_COUNT],
                "APs:%u PMK:%lu ch%u",
                (unsigned)ws.total_aps,
                (unsigned long)es.pmkids_captured,
                ws.current_channel);
        }
        break;
    default:
        break;
    }
}

/* -------------------- button handler -------------------- */

static void button_task(void *arg)
{
    (void)arg;
    QueueHandle_t q = nesso_buttons_event_queue();
    if (!q) { s_button_task = NULL; vTaskDelete(NULL); return; }

    nesso_btn_event_t evt;
    while (1) {
        if (xQueueReceive(q, &evt, portMAX_DELAY) != pdTRUE) continue;

        /*
         * UNIVERSAL CONTROLS:
         *   KEY1 press     = scroll down
         *   KEY2 press     = back
         *   KEY1 long      = emergency stop + main menu
         *   Double-tap     = select (handled by touch callback)
         */

        /* Emergency stop. */
        if (evt.key == NESSO_KEY1 && evt.type == NESSO_BTN_EVT_LONG_PRESS) {
            lvgl_port_lock(0);
            if (s_deauth_active) stop_deauth();
            if (nesso_wifi_beacon_spam_is_active()) nesso_wifi_beacon_spam_stop();
            navigate(UI_MAIN_MENU);
            lvgl_port_unlock();
            continue;
        }

        if (evt.type != NESSO_BTN_EVT_PRESS) continue;

        lvgl_port_lock(0);

        if (evt.key == NESSO_KEY1) {
            /* KEY1 = scroll down everywhere. */
            switch (s_state) {
            case UI_SPLASH:
                navigate(UI_MAIN_MENU);
                break;
            case UI_MAIN_MENU:
            case UI_WIFI_MENU:
            case UI_IR_MENU:
            case UI_SUBGHZ_MENU:
            {
                int count = 0;
                current_menu_items(&count);
                if (count > 0) {
                    s_cursor = (s_cursor + 1) % count;
                    update_menu_cursor(count);
                }
                break;
            }
            case UI_WIFI_AP_LIST:
                s_cursor = (s_cursor + 1) % (s_ap_snap_count > 0 ? (int)s_ap_snap_count : 1);
                refresh_ap_rows(false);
                break;
            case UI_WIFI_DEAUTH_SELECT:
                s_cursor = (s_cursor + 1) % (s_ap_snap_count > 0 ? (int)s_ap_snap_count : 1);
                refresh_ap_rows(true);
                break;
            case UI_WIFI_DEAUTH_ACTIVE:
                break;
            case UI_IR_SAMSUNG_REMOTE:
            {
                s_cursor = (s_cursor + 1) % 10;
                for (int i = 0; i < s_dyn_count && i < 10; ++i)
                    lv_obj_set_style_text_color(s_dyn_labels[i],
                        i == s_cursor ? COL_YELLOW : COL_CYAN, 0);
                break;
            }
            default:
                break;
            }
        } else if (evt.key == NESSO_KEY2) {
            /* KEY2 = back everywhere. Set back flag for slide-right animation. */
            s_nav_back = true;
            switch (s_state) {
            case UI_SPLASH:         navigate(UI_MAIN_MENU); break;
            case UI_MAIN_MENU:      /* top level — no back */ break;
            case UI_WIFI_MENU:      navigate(UI_MAIN_MENU); break;
            case UI_WIFI_SCANNING:  navigate(UI_WIFI_MENU); break;
            case UI_WIFI_AP_LIST:   navigate(UI_WIFI_MENU); break;
            case UI_WIFI_DEAUTH_SELECT: navigate(UI_WIFI_MENU); break;
            case UI_WIFI_DEAUTH_ACTIVE:
                navigate(UI_WIFI_DEAUTH_SELECT); /* back to target list, attack keeps running */
                break;
            case UI_WIFI_BEACON_SPAM:
                nesso_wifi_beacon_spam_stop();
                navigate(UI_WIFI_MENU);
                break;
            case UI_WIFI_DAVEYGOTCHI:
                if (s_deauth_active) stop_deauth();
                navigate(UI_WIFI_MENU);
                break;
            case UI_SUBGHZ_MENU:
                navigate(UI_MAIN_MENU);
                break;
            case UI_SUBGHZ_ANALYZER:
                nesso_buzzer_off();
                navigate(UI_SUBGHZ_MENU);
                break;
            case UI_SUBGHZ_CAPTURE:
            case UI_SUBGHZ_REPLAY:
                navigate(UI_SUBGHZ_MENU);
                break;
            case UI_IR_MENU:
                navigate(UI_MAIN_MENU);
                break;
            case UI_IR_TVBGONE:
                navigate(UI_IR_MENU);
                break;
            case UI_IR_SAMSUNG_REMOTE:
            case UI_IR_VOLUME_MAX:
            case UI_IR_CHANNEL_CHAOS:
                nesso_led(false);
                navigate(UI_IR_MENU);
                break;
            default:
                break;
            }
        }

        lvgl_port_unlock();
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
        .rotation      = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags         = { .buff_dma = true, .swap_bytes = true },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) return ESP_FAIL;

    /* Touch (polling — no GPIO3 interrupt). */
    {
        esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        esp_lcd_panel_io_handle_t tp_io = NULL;
        if (esp_lcd_new_panel_io_i2c(nesso_i2c_bus(), &tp_io_cfg, &tp_io) == ESP_OK) {
            esp_lcd_touch_config_t tp_cfg = {
                .x_max = NESSO_LCD_WIDTH, .y_max = NESSO_LCD_HEIGHT,
                .rst_gpio_num = -1, .int_gpio_num = -1,
                .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
            };
            if (esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_touch) == ESP_OK && s_touch) {
                lvgl_port_touch_cfg_t touch_cfg = { .disp = s_disp, .handle = s_touch };
                s_touch_indev = lvgl_port_add_touch(&touch_cfg);
            }
        }
    }

    lvgl_port_lock(0);
    navigate(UI_SPLASH);
    s_refresh_timer = lv_timer_create(refresh_cb, 250, NULL);
    lvgl_port_unlock();

    xTaskCreate(button_task, "nesso_ui_btn", 6144, NULL, 4, &s_button_task);

    s_up = true;
    ESP_LOGI(TAG, "UI up (splash)");
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

esp_err_t nesso_ui_show(nesso_ui_view_t view) { (void)view; return ESP_OK; }
bool nesso_ui_is_up(void) { return s_up; }
