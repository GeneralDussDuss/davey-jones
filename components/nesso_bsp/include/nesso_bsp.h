/*
 * nesso_bsp — Board Support for Arduino Nesso N1 (DAVEY JONES)
 *
 * Owns the main internal I²C bus and both PI4IOE5V6408 expanders, and exposes
 * named helpers for every peripheral wired through them. Higher-level drivers
 * (LCD, LoRa, buttons) go through this instead of touching the expanders
 * directly, so the "which expander pin is what" knowledge lives in exactly
 * one file.
 *
 * Pin map summary (from the Arduino Nesso N1 full-pinout PDF):
 *
 *   Internal I²C (shared bus):  SDA=GPIO10, SCL=GPIO8
 *     - FT6336 touch       @ 0x38
 *     - BMI270 IMU         @ 0x68
 *     - Expander E0        @ 0x43  (buttons + LoRa control)
 *     - Expander E1        @ 0x44  (power/UI)
 *
 *   E0 pin layout:
 *     P0 = KEY1 (active-low, input w/ pull-up)
 *     P1 = KEY2 (active-low, input w/ pull-up)
 *     P5 = LoRa LNA enable     (output)
 *     P6 = LoRa antenna switch (output, 0=RX path, 1=TX path)
 *     P7 = LoRa reset          (output, active-low)
 *
 *   E1 pin layout:
 *     P0 = POWEROFF command    (output, pulse high to shut down)
 *     P1 = LCD reset           (output, active-low)
 *     P2 = GROVE 5 V enable    (output)
 *     P5 = VIN detect          (input; high = USB plugged in)
 *     P6 = LCD backlight       (output)
 *     P7 = Built-in green LED  (output)
 *
 *   Direct ESP32-C6 GPIO:
 *     GPIO9  = IR TX
 *     GPIO11 = buzzer (PWM)
 *     GPIO3  = touch + IMU INT (shared — poll or attach ISR)
 *
 * License: BSD-3-Clause.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "pi4ioe5v6408.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------- direct-GPIO pin numbers -------------------- */

/* Exposed as macros (not enums) so they can be used in designated initializers. */
#define NESSO_GPIO_I2C_SDA          10
#define NESSO_GPIO_I2C_SCL          8
#define NESSO_GPIO_TOUCH_IMU_INT    3

#define NESSO_GPIO_SPI_MOSI         21
#define NESSO_GPIO_SPI_SCK          20
#define NESSO_GPIO_SPI_MISO         22   /* LoRa only — LCD is write-only */

#define NESSO_GPIO_LCD_CS           17
#define NESSO_GPIO_LCD_DC           16

#define NESSO_GPIO_LORA_CS          23
#define NESSO_GPIO_LORA_BUSY        19
#define NESSO_GPIO_LORA_DIO1        15

#define NESSO_GPIO_BUZZER           11
#define NESSO_GPIO_IR_TX            9

#define NESSO_GPIO_GROVE_SDA        5
#define NESSO_GPIO_GROVE_SCL        4

#define NESSO_GPIO_USB_DM           12
#define NESSO_GPIO_USB_DP           13

#define NESSO_GPIO_HAT_D1           7
#define NESSO_GPIO_HAT_D2           2
#define NESSO_GPIO_HAT_D3           6

/* -------------------- expander pin constants -------------------- */

#define NESSO_E0_PIN_KEY1           0
#define NESSO_E0_PIN_KEY2           1
#define NESSO_E0_PIN_LORA_LNA       5
#define NESSO_E0_PIN_LORA_ANT       6
#define NESSO_E0_PIN_LORA_RESET     7

#define NESSO_E1_PIN_POWEROFF       0
#define NESSO_E1_PIN_LCD_RESET      1
#define NESSO_E1_PIN_GROVE_PWR      2
#define NESSO_E1_PIN_VIN_DETECT     5
#define NESSO_E1_PIN_LCD_BACKLIGHT  6
#define NESSO_E1_PIN_LED            7

/* -------------------- lifecycle -------------------- */

/**
 * Bring up the board's I²C bus, both expanders, and drive every managed pin
 * to a safe known state:
 *
 *   - LCD in reset, backlight off
 *   - LoRa in reset, LNA off, antenna switch to RX path
 *   - LED off
 *   - GROVE 5 V off
 *   - POWEROFF output, low
 *   - KEY1 / KEY2 inputs with pull-ups
 *   - VIN_DETECT input, no pull
 *
 * Must be called exactly once, before any other nesso_* helper.
 */
esp_err_t nesso_bsp_init(void);

/** Shut everything down cleanly (opposite of init). */
esp_err_t nesso_bsp_deinit(void);

/* -------------------- raw handles (for LCD/LoRa drivers) -------------------- */

/** The shared internal I²C bus (FT6336 touch, BMI270 IMU, expanders live here). */
i2c_master_bus_handle_t nesso_i2c_bus(void);

/** Expander E0 @ 0x43 — buttons + LoRa control. */
pi4ioe_handle_t nesso_expander_e0(void);

/** Expander E1 @ 0x44 — power/UI. */
pi4ioe_handle_t nesso_expander_e1(void);

/* -------------------- LCD control -------------------- */

/** Assert (true) or release (false) the LCD reset line (E1.P1, active-low). */
esp_err_t nesso_lcd_reset(bool asserted);

/** Turn the LCD backlight on or off (E1.P6). */
esp_err_t nesso_lcd_backlight(bool on);

/* -------------------- LoRa SX1262 control (power/routing only; SPI is separate) -------------------- */

/*
 * IMPORTANT: the Nesso's SX1262 RF path is a lot simpler than it looks on
 * the pinout. Based on Arduino's working reference code (forum thread
 * "FSK receiver with NESSO N1"), these three expander pins are STATIC
 * enables set once at init, NOT per-operation toggles:
 *
 *   E0.P7 LORA_RESET         — drive HIGH to take the chip out of reset
 *   E0.P5 LORA_LNA_ENABLE    — drive HIGH once, leave on
 *   E0.P6 LORA_RF_SWITCH     — drive HIGH once to power the switch IC
 *
 * TX/RX path selection is done by the SX1262 chip itself via DIO2, enabled
 * through sx126x_set_dio2_as_rf_sw_ctrl(true). The Nesso does NOT use a
 * TCXO — the SX1262 module has an on-board 32 MHz XTAL, so DIO3 stays free.
 */

/** Assert (true) or release (false) the LoRa reset line (E0.P7, active-low). */
esp_err_t nesso_lora_reset(bool asserted);

/**
 * Enable (true) or disable (false) power to the external RF antenna switch
 * IC on E0.P6. Drive this HIGH at init and leave it high — the SX1262
 * drives the TX/RX routing itself via DIO2. Dropping this line cuts all
 * RF in or out, useful for radio-off power saving.
 */
esp_err_t nesso_lora_rf_switch(bool enable);

/**
 * Enable (true) or disable (false) the LoRa receive LNA on E0.P5. Leave
 * HIGH during all normal operation — the antenna switch topology
 * protects it from TX energy. Drop only for deep-sleep power saving.
 */
esp_err_t nesso_lora_lna(bool enabled);

/* -------------------- UI -------------------- */

/** Built-in green LED (E1.P7). */
esp_err_t nesso_led(bool on);

/** Snapshot-read KEY1 (E0.P0). Returns true if pressed (pin reads low). */
bool nesso_key1_pressed(void);

/** Snapshot-read KEY2 (E0.P1). Returns true if pressed (pin reads low). */
bool nesso_key2_pressed(void);

/* -------------------- power / expansion -------------------- */

/**
 * Command a clean shutdown by pulsing POWEROFF (E1.P0) high. The Nesso's
 * external power controller sees the pulse and cuts the rail. This call
 * never returns on success.
 */
esp_err_t nesso_shutdown(void);

/** Enable or disable the 5 V rail on the GROVE connector (E1.P2). */
esp_err_t nesso_grove_power(bool on);

/** Returns true if USB is plugged in (VIN_DETECT = E1.P5 reads high). */
bool nesso_usb_connected(void);

#ifdef __cplusplus
}
#endif
