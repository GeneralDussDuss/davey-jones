/*
 * nesso_lcd.c — ST7789P3 bring-up wrapper for the Nesso N1.
 *
 * Uses ESP-IDF's esp_lcd_panel_st7789 driver but drives reset/backlight
 * through nesso_bsp (E1.P1 / E1.P6 on the expander) instead of direct GPIO.
 */

#include "nesso_lcd.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nesso_bsp.h"
#include "nesso_spi.h"

static const char *TAG = "nesso_lcd";

static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_lcd_panel_handle_t    s_panel     = NULL;

esp_err_t nesso_lcd_init(void)
{
    if (s_panel) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    /* ESP_GOTO_ON_ERROR needs a local `ret` in scope. */
    esp_err_t ret = ESP_OK;

    /* 1. Allocate a panel-IO SPI device on the shared host.
     *    esp_lcd takes the host number directly (cast to opaque handle). */
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = NESSO_GPIO_LCD_CS,
        .dc_gpio_num       = NESSO_GPIO_LCD_DC,
        .spi_mode          = 0,
        .pclk_hz           = NESSO_LCD_SPI_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)NESSO_SPI_HOST,
                                 &io_cfg, &s_io_handle),
        TAG, "esp_lcd_new_panel_io_spi failed");

    /* 2. Pulse reset via the expander. ST7789 needs ≥10 µs low,
     *    then ≥120 ms before you touch it. */
    ESP_GOTO_ON_ERROR(nesso_lcd_reset(true),  fail_after_io, TAG, "assert reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_GOTO_ON_ERROR(nesso_lcd_reset(false), fail_after_io, TAG, "release reset");
    vTaskDelay(pdMS_TO_TICKS(120));

    /* 3. Create the ST7789 panel object.
     *    reset_gpio_num = -1 because we handled reset ourselves above. */
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num   = -1,
        .rgb_ele_order    = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel   = NESSO_LCD_BITS_PER_PIXEL,
    };
    ESP_GOTO_ON_ERROR(
        esp_lcd_new_panel_st7789(s_io_handle, &panel_cfg, &s_panel),
        fail_after_io, TAG, "esp_lcd_new_panel_st7789 failed");

    /* 4. Init sequence + panel config.
     *    ST7789 on the Nesso wants color inversion on; native frame buffer
     *    is 240×320 so we skip the 52/40 non-visible rows+cols via set_gap. */
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(s_panel),
                      fail_after_panel, TAG, "panel init");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true),
                      fail_after_panel, TAG, "invert color");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_set_gap(s_panel,
                                            NESSO_LCD_X_GAP, NESSO_LCD_Y_GAP),
                      fail_after_panel, TAG, "set gap");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true),
                      fail_after_panel, TAG, "display on");

    /* 5. Backlight on via expander. */
    ESP_GOTO_ON_ERROR(nesso_lcd_backlight(true),
                      fail_after_panel, TAG, "backlight on");

    ESP_LOGI(TAG, "ST7789 up: %dx%d, offset (%d,%d)",
             NESSO_LCD_WIDTH, NESSO_LCD_HEIGHT,
             NESSO_LCD_X_GAP, NESSO_LCD_Y_GAP);
    return ESP_OK;

fail_after_panel:
    esp_lcd_panel_del(s_panel);
    s_panel = NULL;
fail_after_io:
    esp_lcd_panel_io_del(s_io_handle);
    s_io_handle = NULL;
    nesso_lcd_reset(true);
    return ESP_FAIL;
}

esp_err_t nesso_lcd_deinit(void)
{
    if (s_panel) {
        esp_lcd_panel_disp_on_off(s_panel, false);
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_io_handle) {
        esp_lcd_panel_io_del(s_io_handle);
        s_io_handle = NULL;
    }
    nesso_lcd_backlight(false);
    nesso_lcd_reset(true);
    return ESP_OK;
}

esp_lcd_panel_handle_t    nesso_lcd_panel(void)    { return s_panel; }
esp_lcd_panel_io_handle_t nesso_lcd_panel_io(void) { return s_io_handle; }
