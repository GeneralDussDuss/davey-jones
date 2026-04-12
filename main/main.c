/*
 * DAVEY JONES — main entry point.
 *
 * Boot sequence (each step is independent; failures are logged but don't
 * brick the following ones):
 *
 *   1. nesso_bsp          I²C bus + both PI4IOE5V6408 expanders
 *   2. nesso_spi          Shared SPI host for LCD + LoRa
 *   3. nesso_lcd          ST7789 panel, backlight on
 *   4. nesso_buttons      Debounced KEY1/KEY2 event queue
 *   5. nesso_ui           LVGL dashboard (consumes buttons)
 *   6. nesso_wifi         STA mode, promisc fanout
 *   7. nesso_wardrive     Channel-hopping beacon → Wigle CSV
 *   8. nesso_eapol        EAPOL PMKID → hashcat 22000
 *   9. nesso_sx1262       LoRa radio (optional — won't block boot)
 *
 * Once everything is up, app_main drops into a 10-second status-print
 * loop. nesso_ui's own task drains the button queue and repaints the
 * LCD from the live counters.
 */

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nesso_bsp.h"
#include "nesso_buttons.h"
#include "nesso_eapol.h"
#include "nesso_lcd.h"
#include "nesso_spi.h"
#include "nesso_sx1262.h"
#include "nesso_ui.h"
#include "nesso_wardrive.h"
#include "nesso_wifi.h"

static const char *TAG = "davey";

/* Try to start a subsystem, log but don't fatal on failure. Returns true
 * on success so the caller can gate dependent subsystems. */
#define TRY(call, name)                                                      \
    ({                                                                       \
        esp_err_t _e = (call);                                               \
        if (_e != ESP_OK) {                                                  \
            ESP_LOGW(TAG, "%-14s skipped: %s", (name), esp_err_to_name(_e)); \
        } else {                                                             \
            ESP_LOGI(TAG, "%-14s ok", (name));                               \
        }                                                                    \
        _e == ESP_OK;                                                        \
    })

void app_main(void)
{
    ESP_LOGI(TAG, "=== DAVEY JONES booting ===");

    /* ----- hardware foundation ----- */
    bool bsp_ok      = TRY(nesso_bsp_init(),     "bsp");
    bool spi_ok      = bsp_ok && TRY(nesso_spi_init(),     "spi");
    bool lcd_ok      = spi_ok && TRY(nesso_lcd_init(),     "lcd");
    bool buttons_ok  = bsp_ok && TRY(nesso_buttons_start(NULL), "buttons");

    /* ----- UI on top of the hardware foundation ----- */
    bool ui_ok       = (lcd_ok && buttons_ok) && TRY(nesso_ui_init(), "ui");

    /* LED triple-blink — confirms we got past the foundation layer even
     * if the LCD is broken. */
    if (bsp_ok) {
        for (int i = 0; i < 3; ++i) {
            nesso_led(true);  vTaskDelay(pdMS_TO_TICKS(80));
            nesso_led(false); vTaskDelay(pdMS_TO_TICKS(80));
        }
    }

    /* ----- recon stack ----- */
    bool wifi_ok     = TRY(nesso_wifi_init(), "wifi");

    if (wifi_ok) {
        nesso_wardrive_config_t wcfg = NESSO_WARDRIVE_CONFIG_DEFAULTS();
        wcfg.dwell_ms = 400;
        wcfg.max_aps  = 256;
        TRY(nesso_wardrive_start(&wcfg), "wardrive");

        nesso_eapol_config_t ecfg = NESSO_EAPOL_CONFIG_DEFAULTS();
        TRY(nesso_eapol_start(&ecfg), "eapol");
    }

    /* ----- LoRa radio (optional) -----
     *
     * This soft-fails on purpose. The Nesso's SX1262 module's TCXO routing
     * isn't verified against a schematic yet, and a wrong TCXO config will
     * cause init to silently fail the radio. If that happens we still want
     * WiFi + UI working, so we just log and continue. */
    nesso_sx1262_config_t lcfg = NESSO_SX1262_CONFIG_DEFAULTS();
    lcfg.freq_hz      = 915000000;  /* US915 default — change per region */
    lcfg.tx_power_dbm = 14;
    TRY(nesso_sx1262_init(&lcfg), "sx1262");

    ESP_LOGI(TAG, "=== boot complete ===");
    if (!ui_ok) {
        ESP_LOGW(TAG, "UI did not start — nothing will be visible on the LCD");
    }

    /* ----- idle: log status every 10 s ----- */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10 * 1000));

        nesso_wardrive_status_t ws = {0};
        nesso_eapol_status_t    es = {0};
        nesso_wardrive_status(&ws);
        nesso_eapol_status(&es);

        ESP_LOGI(TAG,
                 "status: ch=%u APs=%u bcn=%lu pkts=%lu data=%lu pmkid=%lu usb=%s",
                 ws.current_channel,
                 (unsigned)ws.total_aps,
                 (unsigned long)ws.beacons_parsed,
                 (unsigned long)ws.packets_seen,
                 (unsigned long)es.data_frames,
                 (unsigned long)es.pmkids_captured,
                 nesso_usb_connected() ? "Y" : "N");
    }
}
