/*
 * pi4ioe5v6408 — ESP-IDF driver for the Diodes PI4IOE5V6408
 * 8-bit I²C I/O expander.
 *
 * Project: DAVEY JONES (Arduino Nesso N1 pentesting firmware)
 * License: BSD-3-Clause (structural reference: easytarget/pi4ioe5v6408-micropython)
 *
 * The Nesso N1 ships with two of these, internally:
 *   E0 @ 0x43 — KEY1, KEY2, LoRa RESET / ant switch / LNA enable
 *   E1 @ 0x44 — LCD RESET / backlight, GROVE power, VIN detect, LED, POWEROFF
 *
 * Threading: each device handle carries its own mutex. Multiple handles on
 * the same I²C bus are safe; the ESP-IDF i2c_master layer serializes bus
 * access for us.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Number of GPIO pins exposed by the expander. */
#define PI4IOE_NUM_PINS 8

/** I²C 7-bit slave addresses, selected by the chip's ADDR strap. */
#define PI4IOE_ADDR_GND 0x43  /* PI4IOE5V6408-1, ADDR tied to GND */
#define PI4IOE_ADDR_VDD 0x44  /* PI4IOE5V6408-2, ADDR tied to VDD */

/** Register map (from datasheet + easytarget/pi4ioe5v6408-micropython). */
typedef enum {
    PI4IOE_REG_DEVICE_ID_CTRL   = 0x01, /* [7:5]=ID (0b101), [1]=reset flag, [0]=SW reset */
    PI4IOE_REG_IO_DIRECTION     = 0x03, /* 0=input, 1=output */
    PI4IOE_REG_OUTPUT_STATE     = 0x05, /* output drive level */
    PI4IOE_REG_OUTPUT_HI_Z      = 0x07, /* 0=drive, 1=high-Z */
    PI4IOE_REG_INPUT_DEFAULT    = 0x09, /* interrupt reference state */
    PI4IOE_REG_PULL_ENABLE      = 0x0B, /* 0=disabled, 1=enabled */
    PI4IOE_REG_PULL_SELECT      = 0x0D, /* 0=pull-down, 1=pull-up */
    PI4IOE_REG_INPUT_STATUS     = 0x0F, /* RO, reads current input level */
    PI4IOE_REG_INTERRUPT_MASK   = 0x11, /* 0=enabled, 1=masked */
    PI4IOE_REG_INTERRUPT_STATUS = 0x13, /* RO, latched; reading CLEARS ALL */
} pi4ioe_reg_t;

/** Top 3 bits of DEVICE_ID_CTRL must match this for a valid chip. */
#define PI4IOE_DEVICE_ID_MASK  0xE0
#define PI4IOE_DEVICE_ID_VALUE 0xA0  /* 0b101_xxxxx */

/** Direction. */
typedef enum {
    PI4IOE_DIR_INPUT  = 0,
    PI4IOE_DIR_OUTPUT = 1,
} pi4ioe_dir_t;

/** Logical level. */
typedef enum {
    PI4IOE_LEVEL_LOW  = 0,
    PI4IOE_LEVEL_HIGH = 1,
} pi4ioe_level_t;

/** Internal bias selection. */
typedef enum {
    PI4IOE_PULL_NONE = 0,
    PI4IOE_PULL_DOWN,
    PI4IOE_PULL_UP,
} pi4ioe_pull_t;

/** Opaque device handle. */
typedef struct pi4ioe_dev_t *pi4ioe_handle_t;

/** Initialization config. */
typedef struct {
    uint8_t  i2c_address;   /* PI4IOE_ADDR_GND or PI4IOE_ADDR_VDD */
    uint32_t scl_speed_hz;  /* 0 defaults to 400 kHz */
} pi4ioe_config_t;

/* -------------------- lifecycle -------------------- */

/**
 * Attach a PI4IOE5V6408 to an existing i2c_master bus.
 * Does not touch the chip — call pi4ioe_probe() to confirm it's there.
 */
esp_err_t pi4ioe_create(i2c_master_bus_handle_t bus,
                        const pi4ioe_config_t *cfg,
                        pi4ioe_handle_t *out_handle);

/** Detach and free the handle. */
esp_err_t pi4ioe_delete(pi4ioe_handle_t handle);

/**
 * Validate the device ID. On success, if out_was_reset is non-NULL, it
 * will be set true when the chip reports a pending reset flag (bit 1 of
 * DEVICE_ID_CTRL).
 */
esp_err_t pi4ioe_probe(pi4ioe_handle_t handle, bool *out_was_reset);

/** Software reset (writes the SW reset bit). Chip needs re-configuring after. */
esp_err_t pi4ioe_reset(pi4ioe_handle_t handle);

/* -------------------- per-pin config -------------------- */

esp_err_t pi4ioe_set_direction(pi4ioe_handle_t handle, uint8_t pin, pi4ioe_dir_t dir);
esp_err_t pi4ioe_set_level   (pi4ioe_handle_t handle, uint8_t pin, pi4ioe_level_t level);
esp_err_t pi4ioe_get_level   (pi4ioe_handle_t handle, uint8_t pin, pi4ioe_level_t *out_level);
esp_err_t pi4ioe_set_pull    (pi4ioe_handle_t handle, uint8_t pin, pi4ioe_pull_t pull);
esp_err_t pi4ioe_set_hi_z    (pi4ioe_handle_t handle, uint8_t pin, bool high_z);

/* -------------------- one-shot configuration -------------------- */

/**
 * Configure a pin as a push-pull output at the given initial level.
 * Internally clears high-Z, sets the output state, then flips direction,
 * per the chip's documented ordering requirement (easytarget driver, §output).
 */
esp_err_t pi4ioe_configure_output(pi4ioe_handle_t handle,
                                  uint8_t pin,
                                  pi4ioe_level_t initial);

/**
 * Configure a pin as an input with the given bias.
 * Clears the output drive before flipping direction to avoid glitches.
 */
esp_err_t pi4ioe_configure_input(pi4ioe_handle_t handle,
                                 uint8_t pin,
                                 pi4ioe_pull_t pull);

/* -------------------- whole-port helpers -------------------- */

/** Read all 8 input bits at once. */
esp_err_t pi4ioe_read_input_port(pi4ioe_handle_t handle, uint8_t *out_value);

/** Write all 8 output bits at once. Does not touch direction. */
esp_err_t pi4ioe_write_output_port(pi4ioe_handle_t handle, uint8_t value);

/* -------------------- interrupts -------------------- */

/**
 * Set which pins drive the external INT pin. Takes a mask where
 * bit N == 1 means "pin N may raise the INT line". This function
 * inverts the mask internally to match the chip's "1=masked" convention.
 *
 * NOTE: the chip's INT line is global — the mask only gates which pins
 * can raise it, but the STATUS register latches every change regardless.
 */
esp_err_t pi4ioe_set_interrupt_mask(pi4ioe_handle_t handle, uint8_t enable_mask);

/**
 * Read the latched interrupt status and CLEAR it as a side effect.
 * The chip cannot clear individual bits — every read drops all flags.
 */
esp_err_t pi4ioe_read_interrupt_status(pi4ioe_handle_t handle, uint8_t *out_flags);

/* -------------------- raw escape hatches -------------------- */

esp_err_t pi4ioe_read_reg (pi4ioe_handle_t handle, pi4ioe_reg_t reg, uint8_t *out_value);
esp_err_t pi4ioe_write_reg(pi4ioe_handle_t handle, pi4ioe_reg_t reg, uint8_t value);

/** Dump every register at ESP_LOG_INFO (for first bring-up). */
esp_err_t pi4ioe_dump(pi4ioe_handle_t handle);

#ifdef __cplusplus
}
#endif
