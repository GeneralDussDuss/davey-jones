/*
 * pi4ioe5v6408.c — Diodes PI4IOE5V6408 driver for ESP-IDF 5.3+
 *
 * Part of DAVEY JONES (Arduino Nesso N1 pentesting firmware).
 * License: BSD-3-Clause.
 */

#include "pi4ioe5v6408.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "pi4ioe";

/* ESP-IDF I²C transfer timeout. 100 ms is generous for single-byte ops. */
#define PI4IOE_I2C_TIMEOUT_MS 100

/* Default I²C clock if the caller passes 0. */
#define PI4IOE_DEFAULT_SCL_HZ 400000

struct pi4ioe_dev_t {
    i2c_master_dev_handle_t i2c_dev;
    uint8_t                 address;
    SemaphoreHandle_t       mutex;
};

/* -------------------- low-level register helpers (mutex NOT held) -------------------- */

static esp_err_t reg_read(pi4ioe_handle_t h, pi4ioe_reg_t reg, uint8_t *out)
{
    uint8_t addr = (uint8_t)reg;
    return i2c_master_transmit_receive(h->i2c_dev, &addr, 1, out, 1,
                                       PI4IOE_I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(pi4ioe_handle_t h, pi4ioe_reg_t reg, uint8_t value)
{
    uint8_t buf[2] = { (uint8_t)reg, value };
    return i2c_master_transmit(h->i2c_dev, buf, sizeof(buf),
                               PI4IOE_I2C_TIMEOUT_MS);
}

/*
 * Read-modify-write a single bit in a register. Caller must hold the mutex.
 * Skips the write if the target state is already correct — saves an I²C
 * transaction during polling loops.
 */
static esp_err_t reg_update_bit(pi4ioe_handle_t h,
                                pi4ioe_reg_t reg,
                                uint8_t pin,
                                bool value)
{
    uint8_t cur = 0;
    esp_err_t err = reg_read(h, reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t mask = (uint8_t)(1U << pin);
    uint8_t next = value ? (uint8_t)(cur | mask) : (uint8_t)(cur & ~mask);
    if (next == cur) {
        return ESP_OK;
    }
    return reg_write(h, reg, next);
}

/* Short locking helpers to keep the API implementations readable. */
#define LOCK(h)                                                               \
    do {                                                                      \
        if (xSemaphoreTake((h)->mutex, portMAX_DELAY) != pdTRUE) {            \
            return ESP_ERR_TIMEOUT;                                           \
        }                                                                     \
    } while (0)

#define UNLOCK(h) xSemaphoreGive((h)->mutex)

/* -------------------- lifecycle -------------------- */

esp_err_t pi4ioe_create(i2c_master_bus_handle_t bus,
                        const pi4ioe_config_t *cfg,
                        pi4ioe_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(bus && cfg && out_handle,
                        ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_FALSE(cfg->i2c_address == PI4IOE_ADDR_GND ||
                            cfg->i2c_address == PI4IOE_ADDR_VDD,
                        ESP_ERR_INVALID_ARG, TAG,
                        "addr must be 0x43 or 0x44, got 0x%02x",
                        cfg->i2c_address);

    struct pi4ioe_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }

    dev->address = cfg->i2c_address;
    dev->mutex   = xSemaphoreCreateMutex();
    if (!dev->mutex) {
        free(dev);
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t i2c_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = cfg->i2c_address,
        .scl_speed_hz    = cfg->scl_speed_hz ? cfg->scl_speed_hz
                                             : PI4IOE_DEFAULT_SCL_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &i2c_cfg, &dev->i2c_dev);
    if (err != ESP_OK) {
        vSemaphoreDelete(dev->mutex);
        free(dev);
        return err;
    }

    *out_handle = dev;
    ESP_LOGD(TAG, "created device @0x%02x", cfg->i2c_address);
    return ESP_OK;
}

esp_err_t pi4ioe_delete(pi4ioe_handle_t h)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    if (h->i2c_dev) {
        i2c_master_bus_rm_device(h->i2c_dev);
    }
    if (h->mutex) {
        vSemaphoreDelete(h->mutex);
    }
    free(h);
    return ESP_OK;
}

esp_err_t pi4ioe_probe(pi4ioe_handle_t h, bool *out_was_reset)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    LOCK(h);
    uint8_t v = 0;
    esp_err_t err = reg_read(h, PI4IOE_REG_DEVICE_ID_CTRL, &v);
    if (err == ESP_OK) {
        if ((v & PI4IOE_DEVICE_ID_MASK) != PI4IOE_DEVICE_ID_VALUE) {
            ESP_LOGE(TAG,
                     "0x%02x: bad device ID (got 0x%02x, want top3=0b101)",
                     h->address, v);
            err = ESP_ERR_NOT_SUPPORTED;
        } else if (out_was_reset) {
            *out_was_reset = (v & 0x02) != 0;
        }
    } else {
        ESP_LOGE(TAG, "0x%02x: probe I²C error: %s",
                 h->address, esp_err_to_name(err));
    }
    UNLOCK(h);
    return err;
}

esp_err_t pi4ioe_reset(pi4ioe_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    LOCK(h);
    /* Bit 0 of DEVICE_ID_CTRL triggers a software reset. */
    esp_err_t err = reg_write(h, PI4IOE_REG_DEVICE_ID_CTRL, 0x01);
    UNLOCK(h);
    return err;
}

/* -------------------- per-pin config -------------------- */

esp_err_t pi4ioe_set_direction(pi4ioe_handle_t h, uint8_t pin, pi4ioe_dir_t dir)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_FALSE(pin < PI4IOE_NUM_PINS, ESP_ERR_INVALID_ARG, TAG,
                        "pin %u out of range", pin);
    LOCK(h);
    esp_err_t err = reg_update_bit(h, PI4IOE_REG_IO_DIRECTION, pin,
                                   dir == PI4IOE_DIR_OUTPUT);
    UNLOCK(h);
    return err;
}

esp_err_t pi4ioe_set_level(pi4ioe_handle_t h, uint8_t pin, pi4ioe_level_t level)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_FALSE(pin < PI4IOE_NUM_PINS, ESP_ERR_INVALID_ARG, TAG,
                        "pin %u out of range", pin);
    LOCK(h);
    esp_err_t err = reg_update_bit(h, PI4IOE_REG_OUTPUT_STATE, pin,
                                   level == PI4IOE_LEVEL_HIGH);
    UNLOCK(h);
    return err;
}

esp_err_t pi4ioe_get_level(pi4ioe_handle_t h, uint8_t pin,
                           pi4ioe_level_t *out_level)
{
    ESP_RETURN_ON_FALSE(h && out_level, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_FALSE(pin < PI4IOE_NUM_PINS, ESP_ERR_INVALID_ARG, TAG,
                        "pin %u out of range", pin);
    LOCK(h);
    uint8_t v = 0;
    esp_err_t err = reg_read(h, PI4IOE_REG_INPUT_STATUS, &v);
    if (err == ESP_OK) {
        *out_level = (v & (1U << pin)) ? PI4IOE_LEVEL_HIGH : PI4IOE_LEVEL_LOW;
    }
    UNLOCK(h);
    return err;
}

esp_err_t pi4ioe_set_pull(pi4ioe_handle_t h, uint8_t pin, pi4ioe_pull_t pull)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_FALSE(pin < PI4IOE_NUM_PINS, ESP_ERR_INVALID_ARG, TAG,
                        "pin %u out of range", pin);
    LOCK(h);
    esp_err_t err = ESP_OK;
    if (pull == PI4IOE_PULL_NONE) {
        err = reg_update_bit(h, PI4IOE_REG_PULL_ENABLE, pin, false);
    } else {
        err = reg_update_bit(h, PI4IOE_REG_PULL_SELECT, pin,
                             pull == PI4IOE_PULL_UP);
        if (err == ESP_OK) {
            err = reg_update_bit(h, PI4IOE_REG_PULL_ENABLE, pin, true);
        }
    }
    UNLOCK(h);
    return err;
}

esp_err_t pi4ioe_set_hi_z(pi4ioe_handle_t h, uint8_t pin, bool high_z)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_FALSE(pin < PI4IOE_NUM_PINS, ESP_ERR_INVALID_ARG, TAG,
                        "pin %u out of range", pin);
    LOCK(h);
    esp_err_t err = reg_update_bit(h, PI4IOE_REG_OUTPUT_HI_Z, pin, high_z);
    UNLOCK(h);
    return err;
}

/* -------------------- one-shot configuration -------------------- */

esp_err_t pi4ioe_configure_output(pi4ioe_handle_t h, uint8_t pin,
                                  pi4ioe_level_t initial)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_FALSE(pin < PI4IOE_NUM_PINS, ESP_ERR_INVALID_ARG, TAG,
                        "pin %u out of range", pin);
    LOCK(h);
    esp_err_t err;
    /* Order: clear high-Z, set output state, flip direction. */
    err = reg_update_bit(h, PI4IOE_REG_OUTPUT_HI_Z, pin, false);
    if (err != ESP_OK) goto out;
    err = reg_update_bit(h, PI4IOE_REG_OUTPUT_STATE, pin,
                         initial == PI4IOE_LEVEL_HIGH);
    if (err != ESP_OK) goto out;
    err = reg_update_bit(h, PI4IOE_REG_IO_DIRECTION, pin, true);
out:
    UNLOCK(h);
    return err;
}

esp_err_t pi4ioe_configure_input(pi4ioe_handle_t h, uint8_t pin,
                                 pi4ioe_pull_t pull)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_FALSE(pin < PI4IOE_NUM_PINS, ESP_ERR_INVALID_ARG, TAG,
                        "pin %u out of range", pin);
    LOCK(h);
    esp_err_t err;
    /* Drop output drive before direction flip to avoid glitches. */
    err = reg_update_bit(h, PI4IOE_REG_OUTPUT_HI_Z, pin, true);
    if (err != ESP_OK) goto out;
    err = reg_update_bit(h, PI4IOE_REG_OUTPUT_STATE, pin, false);
    if (err != ESP_OK) goto out;
    err = reg_update_bit(h, PI4IOE_REG_IO_DIRECTION, pin, false);
    if (err != ESP_OK) goto out;

    if (pull == PI4IOE_PULL_NONE) {
        err = reg_update_bit(h, PI4IOE_REG_PULL_ENABLE, pin, false);
    } else {
        err = reg_update_bit(h, PI4IOE_REG_PULL_SELECT, pin,
                             pull == PI4IOE_PULL_UP);
        if (err == ESP_OK) {
            err = reg_update_bit(h, PI4IOE_REG_PULL_ENABLE, pin, true);
        }
    }
out:
    UNLOCK(h);
    return err;
}

/* -------------------- whole-port helpers -------------------- */

esp_err_t pi4ioe_read_input_port(pi4ioe_handle_t h, uint8_t *out)
{
    ESP_RETURN_ON_FALSE(h && out, ESP_ERR_INVALID_ARG, TAG, "null arg");
    LOCK(h);
    esp_err_t err = reg_read(h, PI4IOE_REG_INPUT_STATUS, out);
    UNLOCK(h);
    return err;
}

esp_err_t pi4ioe_write_output_port(pi4ioe_handle_t h, uint8_t value)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    LOCK(h);
    esp_err_t err = reg_write(h, PI4IOE_REG_OUTPUT_STATE, value);
    UNLOCK(h);
    return err;
}

/* -------------------- interrupts -------------------- */

esp_err_t pi4ioe_set_interrupt_mask(pi4ioe_handle_t h, uint8_t enable_mask)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    LOCK(h);
    /* Chip wants 1=masked, we take user-facing 1=enabled. Invert. */
    esp_err_t err = reg_write(h, PI4IOE_REG_INTERRUPT_MASK,
                              (uint8_t)~enable_mask);
    UNLOCK(h);
    return err;
}

esp_err_t pi4ioe_read_interrupt_status(pi4ioe_handle_t h, uint8_t *out_flags)
{
    ESP_RETURN_ON_FALSE(h && out_flags, ESP_ERR_INVALID_ARG, TAG, "null arg");
    LOCK(h);
    /* Reading clears ALL latched flags — chip quirk, not an API choice. */
    esp_err_t err = reg_read(h, PI4IOE_REG_INTERRUPT_STATUS, out_flags);
    UNLOCK(h);
    return err;
}

/* -------------------- raw escape hatches -------------------- */

esp_err_t pi4ioe_read_reg(pi4ioe_handle_t h, pi4ioe_reg_t reg, uint8_t *out)
{
    ESP_RETURN_ON_FALSE(h && out, ESP_ERR_INVALID_ARG, TAG, "null arg");
    LOCK(h);
    esp_err_t err = reg_read(h, reg, out);
    UNLOCK(h);
    return err;
}

esp_err_t pi4ioe_write_reg(pi4ioe_handle_t h, pi4ioe_reg_t reg, uint8_t value)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    LOCK(h);
    esp_err_t err = reg_write(h, reg, value);
    UNLOCK(h);
    return err;
}

/* -------------------- debug dump -------------------- */

esp_err_t pi4ioe_dump(pi4ioe_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");

    static const struct {
        pi4ioe_reg_t reg;
        const char  *name;
    } table[] = {
        { PI4IOE_REG_DEVICE_ID_CTRL,   "DEVICE_ID_CTRL  " },
        { PI4IOE_REG_IO_DIRECTION,     "IO_DIRECTION    " },
        { PI4IOE_REG_OUTPUT_STATE,     "OUTPUT_STATE    " },
        { PI4IOE_REG_OUTPUT_HI_Z,      "OUTPUT_HI_Z     " },
        { PI4IOE_REG_INPUT_DEFAULT,    "INPUT_DEFAULT   " },
        { PI4IOE_REG_PULL_ENABLE,      "PULL_ENABLE     " },
        { PI4IOE_REG_PULL_SELECT,      "PULL_SELECT     " },
        { PI4IOE_REG_INPUT_STATUS,     "INPUT_STATUS    " },
        { PI4IOE_REG_INTERRUPT_MASK,   "INTERRUPT_MASK  " },
        { PI4IOE_REG_INTERRUPT_STATUS, "INTERRUPT_STATUS" },
    };

    ESP_LOGI(TAG, "=== PI4IOE5V6408 @ 0x%02x dump ===", h->address);
    LOCK(h);
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        uint8_t v = 0;
        esp_err_t err = reg_read(h, table[i].reg, &v);
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "  0x%02x %s = 0x%02x  %c%c%c%c%c%c%c%c",
                     table[i].reg, table[i].name, v,
                     (v & 0x80) ? '1' : '0',
                     (v & 0x40) ? '1' : '0',
                     (v & 0x20) ? '1' : '0',
                     (v & 0x10) ? '1' : '0',
                     (v & 0x08) ? '1' : '0',
                     (v & 0x04) ? '1' : '0',
                     (v & 0x02) ? '1' : '0',
                     (v & 0x01) ? '1' : '0');
        } else {
            ESP_LOGW(TAG, "  0x%02x %s = <read err %s>",
                     table[i].reg, table[i].name, esp_err_to_name(err));
        }
    }
    UNLOCK(h);
    return ESP_OK;
}
