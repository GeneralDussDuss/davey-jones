/*
 * nesso_bsp.c — Arduino Nesso N1 board support for DAVEY JONES.
 *
 * Owns the main internal I²C bus + both PI4IOE5V6408 expanders and exposes
 * named helpers so the rest of the firmware never has to remember which
 * expander holds which pin.
 */

#include "nesso_bsp.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nesso_bsp";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static pi4ioe_handle_t         s_e0      = NULL;  /* 0x43 — buttons + LoRa */
static pi4ioe_handle_t         s_e1      = NULL;  /* 0x44 — power/UI */
static bool                    s_ready   = false;

/* -------------------- lifecycle -------------------- */

/* Cleanup everything allocated so far. Safe to call on any partial state. */
static void bsp_cleanup(void)
{
    if (s_e0)      { pi4ioe_delete(s_e0);            s_e0      = NULL; }
    if (s_e1)      { pi4ioe_delete(s_e1);            s_e1      = NULL; }
    if (s_i2c_bus) { i2c_del_master_bus(s_i2c_bus);  s_i2c_bus = NULL; }
}

/* Local helper: bail out with cleanup + return err. */
#define BSP_TRY(expr, msg)                                                    \
    do {                                                                      \
        esp_err_t __e = (expr);                                               \
        if (__e != ESP_OK) {                                                  \
            ESP_LOGE(TAG, "%s: %s", (msg), esp_err_to_name(__e));             \
            err = __e;                                                        \
            goto fail;                                                        \
        }                                                                     \
    } while (0)

esp_err_t nesso_bsp_init(void)
{
    if (s_ready) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;

    /* 1. Main internal I²C bus. */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = I2C_NUM_0,
        .sda_io_num                   = NESSO_GPIO_I2C_SDA,
        .scl_io_num                   = NESSO_GPIO_I2C_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    BSP_TRY(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), "i2c_new_master_bus");

    /* 2. Attach both expanders. */
    pi4ioe_config_t e0_cfg = {
        .i2c_address  = PI4IOE_ADDR_GND,
        .scl_speed_hz = 400000,
    };
    pi4ioe_config_t e1_cfg = {
        .i2c_address  = PI4IOE_ADDR_VDD,
        .scl_speed_hz = 400000,
    };
    BSP_TRY(pi4ioe_create(s_i2c_bus, &e0_cfg, &s_e0), "create E0");
    BSP_TRY(pi4ioe_create(s_i2c_bus, &e1_cfg, &s_e1), "create E1");

    /* 3. Probe both before we trust them. */
    bool e0_reset = false, e1_reset = false;
    BSP_TRY(pi4ioe_probe(s_e0, &e0_reset), "E0 probe");
    BSP_TRY(pi4ioe_probe(s_e1, &e1_reset), "E1 probe");
    ESP_LOGI(TAG, "expanders alive: E0 reset_flag=%d, E1 reset_flag=%d",
             e0_reset, e1_reset);

    /* 4. Drive E0 to a safe state.
     *
     *    KEY1 / KEY2     — inputs with pull-up (buttons active-low to GND)
     *    LoRa LNA        — output HIGH (LNA on, left permanently enabled
     *                      per the Arduino working reference code — the
     *                      antenna switch topology protects it from TX)
     *    LoRa RF_SWITCH  — output HIGH (antenna switch IC powered, actual
     *                      TX/RX routing is done by SX1262 DIO2)
     *    LoRa RESET      — output LOW (hold SX1262 in reset until the
     *                      sx1262 driver brings it up)
     */
    BSP_TRY(pi4ioe_configure_input (s_e0, NESSO_E0_PIN_KEY1,       PI4IOE_PULL_UP),    "E0 KEY1");
    BSP_TRY(pi4ioe_configure_input (s_e0, NESSO_E0_PIN_KEY2,       PI4IOE_PULL_UP),    "E0 KEY2");
    BSP_TRY(pi4ioe_configure_output(s_e0, NESSO_E0_PIN_LORA_LNA,   PI4IOE_LEVEL_HIGH), "E0 LoRa LNA");
    BSP_TRY(pi4ioe_configure_output(s_e0, NESSO_E0_PIN_LORA_ANT,   PI4IOE_LEVEL_HIGH), "E0 LoRa RF switch");
    BSP_TRY(pi4ioe_configure_output(s_e0, NESSO_E0_PIN_LORA_RESET, PI4IOE_LEVEL_LOW),  "E0 LoRa RESET");

    /* 5. Drive E1 to a safe state.
     *
     *    POWEROFF      — output low (don't shut down by accident)
     *    LCD RESET     — output low (LCD held in reset until LCD driver releases)
     *    GROVE power   — output low (5 V rail off)
     *    VIN detect    — input, no pull (pin is driven by the charger path)
     *    LCD backlight — output low (off)
     *    LED           — output low (off)
     */
    BSP_TRY(pi4ioe_configure_output(s_e1, NESSO_E1_PIN_POWEROFF,      PI4IOE_LEVEL_LOW), "E1 POWEROFF");
    BSP_TRY(pi4ioe_configure_output(s_e1, NESSO_E1_PIN_LCD_RESET,     PI4IOE_LEVEL_LOW), "E1 LCD_RESET");
    BSP_TRY(pi4ioe_configure_output(s_e1, NESSO_E1_PIN_GROVE_PWR,     PI4IOE_LEVEL_LOW), "E1 GROVE_PWR");
    BSP_TRY(pi4ioe_configure_input (s_e1, NESSO_E1_PIN_VIN_DETECT,    PI4IOE_PULL_NONE), "E1 VIN_DETECT");
    BSP_TRY(pi4ioe_configure_output(s_e1, NESSO_E1_PIN_LCD_BACKLIGHT, PI4IOE_LEVEL_LOW), "E1 LCD_BACKLIGHT");
    BSP_TRY(pi4ioe_configure_output(s_e1, NESSO_E1_PIN_LED,           PI4IOE_LEVEL_LOW), "E1 LED");

    s_ready = true;
    ESP_LOGI(TAG, "board ready");
    return ESP_OK;

fail:
    bsp_cleanup();
    return err;
}

#undef BSP_TRY

esp_err_t nesso_bsp_deinit(void)
{
    if (!s_ready) return ESP_OK;
    bsp_cleanup();
    s_ready = false;
    return ESP_OK;
}

/* -------------------- raw handles -------------------- */

i2c_master_bus_handle_t nesso_i2c_bus(void) { return s_i2c_bus; }
pi4ioe_handle_t         nesso_expander_e0(void) { return s_e0; }
pi4ioe_handle_t         nesso_expander_e1(void) { return s_e1; }

/* -------------------- LCD -------------------- */

esp_err_t nesso_lcd_reset(bool asserted)
{
    /* LCD reset is active-low: asserted=true → drive LOW. */
    return pi4ioe_set_level(s_e1, NESSO_E1_PIN_LCD_RESET,
                            asserted ? PI4IOE_LEVEL_LOW : PI4IOE_LEVEL_HIGH);
}

esp_err_t nesso_lcd_backlight(bool on)
{
    return pi4ioe_set_level(s_e1, NESSO_E1_PIN_LCD_BACKLIGHT,
                            on ? PI4IOE_LEVEL_HIGH : PI4IOE_LEVEL_LOW);
}

/* -------------------- LoRa control -------------------- */

esp_err_t nesso_lora_reset(bool asserted)
{
    /* LoRa reset is active-low. */
    return pi4ioe_set_level(s_e0, NESSO_E0_PIN_LORA_RESET,
                            asserted ? PI4IOE_LEVEL_LOW : PI4IOE_LEVEL_HIGH);
}

esp_err_t nesso_lora_rf_switch(bool enable)
{
    /* Drives the external RF switch IC's power. Leave HIGH under normal
     * operation; actual TX/RX routing is done by SX1262 DIO2. */
    return pi4ioe_set_level(s_e0, NESSO_E0_PIN_LORA_ANT,
                            enable ? PI4IOE_LEVEL_HIGH : PI4IOE_LEVEL_LOW);
}

esp_err_t nesso_lora_lna(bool enabled)
{
    return pi4ioe_set_level(s_e0, NESSO_E0_PIN_LORA_LNA,
                            enabled ? PI4IOE_LEVEL_HIGH : PI4IOE_LEVEL_LOW);
}

/* -------------------- UI -------------------- */

esp_err_t nesso_led(bool on)
{
    return pi4ioe_set_level(s_e1, NESSO_E1_PIN_LED,
                            on ? PI4IOE_LEVEL_HIGH : PI4IOE_LEVEL_LOW);
}

bool nesso_key1_pressed(void)
{
    pi4ioe_level_t lvl;
    if (pi4ioe_get_level(s_e0, NESSO_E0_PIN_KEY1, &lvl) != ESP_OK) {
        return false;
    }
    return lvl == PI4IOE_LEVEL_LOW;  /* active-low with pull-up */
}

bool nesso_key2_pressed(void)
{
    pi4ioe_level_t lvl;
    if (pi4ioe_get_level(s_e0, NESSO_E0_PIN_KEY2, &lvl) != ESP_OK) {
        return false;
    }
    return lvl == PI4IOE_LEVEL_LOW;
}

/* -------------------- power / expansion -------------------- */

esp_err_t nesso_shutdown(void)
{
    /*
     * Pulse POWEROFF (E1.P0) high. The Nesso's external power controller
     * watches this line and cuts the rail. On success, control never
     * returns — the chip loses power mid-task.
     */
    ESP_LOGI(TAG, "commanding shutdown via E1.P0");
    esp_err_t err = pi4ioe_set_level(s_e1, NESSO_E1_PIN_POWEROFF, PI4IOE_LEVEL_HIGH);
    if (err != ESP_OK) return err;

    /* Give the power controller a beat to react. If we're still alive after,
     * something went wrong — drop the line back and return an error. */
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_LOGW(TAG, "shutdown did not cut power; resetting POWEROFF line");
    (void)pi4ioe_set_level(s_e1, NESSO_E1_PIN_POWEROFF, PI4IOE_LEVEL_LOW);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t nesso_grove_power(bool on)
{
    return pi4ioe_set_level(s_e1, NESSO_E1_PIN_GROVE_PWR,
                            on ? PI4IOE_LEVEL_HIGH : PI4IOE_LEVEL_LOW);
}

bool nesso_usb_connected(void)
{
    pi4ioe_level_t lvl;
    if (pi4ioe_get_level(s_e1, NESSO_E1_PIN_VIN_DETECT, &lvl) != ESP_OK) {
        return false;
    }
    return lvl == PI4IOE_LEVEL_HIGH;
}
