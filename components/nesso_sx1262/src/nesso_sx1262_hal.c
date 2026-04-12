/*
 * nesso_sx1262_hal.c — Semtech SX1262 HAL implementation for the Nesso N1.
 *
 * Implements the four sx126x_hal_* functions that Semtech's reference C
 * driver expects users to provide:
 *
 *     sx126x_hal_write(ctx, cmd, cmd_len, data, data_len)
 *     sx126x_hal_read (ctx, cmd, cmd_len, data, data_len)
 *     sx126x_hal_reset(ctx)
 *     sx126x_hal_wakeup(ctx)
 *
 * The `context` pointer is our private nesso_sx1262_hal_ctx_t which carries
 * the SPI device handle plus the BUSY GPIO number. All other Nesso pin
 * control (reset line, antenna switch, LNA) lives on the I²C expander and
 * is touched through nesso_bsp from the higher-level wrapper — NOT from
 * inside the HAL callbacks.
 *
 * Contract the Semtech driver assumes:
 *   - Every HAL call returns with BUSY low and the chip idle.
 *   - BUSY polling is the HAL's problem; the driver never checks it itself.
 *
 * If BUSY fails to go low within BUSY_TIMEOUT_MS, every HAL call returns
 * SX126X_HAL_STATUS_ERROR and the caller has to decide what to do.
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"           /* esp_rom_delay_us */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nesso_bsp.h"
#include "nesso_sx1262_hal.h"      /* private — in src/ */
#include "sx126x_hal.h"
#include "sx126x_status.h"

static const char *TAG = "sx126x_hal";

/*
 * Maximum combined command+data length per transaction.
 * SX126x commands are ≤ 16 bytes of opcode/args, payloads up to 255 bytes.
 * Round up with margin.
 */
#define HAL_MAX_XFER_BYTES  288

/* Max time we'll wait for BUSY to drop after any transaction. */
#define BUSY_TIMEOUT_MS     100

/* -------------------- helpers -------------------- */

static inline bool busy_is_high(const nesso_sx1262_hal_ctx_t *ctx)
{
    return gpio_get_level(ctx->busy_gpio) != 0;
}

static sx126x_hal_status_t wait_busy_low(const nesso_sx1262_hal_ctx_t *ctx)
{
    /* Fast path: tight spin for the first 500 µs. Most commands complete
     * within a few microseconds — we don't want the RTOS scheduler overhead
     * of vTaskDelay for those. After the fast window we fall back to
     * vTaskDelay so we don't hog the CPU during long calibrations. */
    for (int i = 0; i < 500; ++i) {
        if (!busy_is_high(ctx)) return SX126X_HAL_STATUS_OK;
        esp_rom_delay_us(1);
    }

    TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(BUSY_TIMEOUT_MS);
    while (busy_is_high(ctx)) {
        if ((xTaskGetTickCount() - start) > limit) {
            ESP_LOGE(TAG, "BUSY stuck high for >%d ms", BUSY_TIMEOUT_MS);
            return SX126X_HAL_STATUS_ERROR;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return SX126X_HAL_STATUS_OK;
}

/* -------------------- sx126x_hal_write -------------------- */

sx126x_hal_status_t sx126x_hal_write(const void *context,
                                     const uint8_t *command,
                                     const uint16_t command_length,
                                     const uint8_t *data,
                                     const uint16_t data_length)
{
    const nesso_sx1262_hal_ctx_t *ctx = (const nesso_sx1262_hal_ctx_t *)context;
    if (!ctx || !ctx->spi || (command_length && !command)) {
        return SX126X_HAL_STATUS_ERROR;
    }

    const uint16_t total = command_length + data_length;
    if (total == 0 || total > HAL_MAX_XFER_BYTES) {
        ESP_LOGE(TAG, "write length %u out of range", total);
        return SX126X_HAL_STATUS_ERROR;
    }

    /* 1. Wait for BUSY low before starting. */
    if (wait_busy_low(ctx) != SX126X_HAL_STATUS_OK) {
        return SX126X_HAL_STATUS_ERROR;
    }

    /* 2. Pack command + data into a single contiguous TX buffer so we can
     *    issue one SPI transaction (CS held low across the whole frame,
     *    which is what the SX126x expects). */
    uint8_t tx[HAL_MAX_XFER_BYTES];
    memcpy(tx, command, command_length);
    if (data_length && data) {
        memcpy(tx + command_length, data, data_length);
    }

    spi_transaction_t t = {
        .length    = (size_t)total * 8,   /* bits */
        .tx_buffer = tx,
        .rx_buffer = NULL,
    };
    if (spi_device_transmit(ctx->spi, &t) != ESP_OK) {
        ESP_LOGE(TAG, "spi_device_transmit failed in write");
        return SX126X_HAL_STATUS_ERROR;
    }

    /* 3. Wait BUSY low again before returning — the chip will have raised
     *    it during the transaction and must drop it before we issue the
     *    next command. */
    return wait_busy_low(ctx);
}

/* -------------------- sx126x_hal_read -------------------- */

sx126x_hal_status_t sx126x_hal_read(const void *context,
                                    const uint8_t *command,
                                    const uint16_t command_length,
                                    uint8_t *data,
                                    const uint16_t data_length)
{
    const nesso_sx1262_hal_ctx_t *ctx = (const nesso_sx1262_hal_ctx_t *)context;
    if (!ctx || !ctx->spi || (command_length && !command) ||
        (data_length && !data)) {
        return SX126X_HAL_STATUS_ERROR;
    }

    const uint16_t total = command_length + data_length;
    if (total == 0 || total > HAL_MAX_XFER_BYTES) {
        ESP_LOGE(TAG, "read length %u out of range", total);
        return SX126X_HAL_STATUS_ERROR;
    }

    /* 1. Wait BUSY low. */
    if (wait_busy_low(ctx) != SX126X_HAL_STATUS_OK) {
        return SX126X_HAL_STATUS_ERROR;
    }

    /*
     * 2. SX126x read pattern: clock out the command bytes (opcode + any
     *    args), then clock out `data_length` bytes of NOPs while clocking
     *    IN the real response. Both halves happen in one CS-low period.
     *
     *    We do a single full-duplex transaction:
     *      - tx_buffer: [command ..., NOP, NOP, ...]
     *      - rx_buffer: [junk ..., real response bytes]
     *    Then copy the tail of the rx buffer into the caller's `data`.
     */
    uint8_t tx[HAL_MAX_XFER_BYTES];
    uint8_t rx[HAL_MAX_XFER_BYTES];
    memset(tx, SX126X_NOP, total);
    memcpy(tx, command, command_length);

    spi_transaction_t t = {
        .length    = (size_t)total * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    if (spi_device_transmit(ctx->spi, &t) != ESP_OK) {
        ESP_LOGE(TAG, "spi_device_transmit failed in read");
        return SX126X_HAL_STATUS_ERROR;
    }

    if (data_length) {
        memcpy(data, rx + command_length, data_length);
    }

    /* 3. Wait BUSY low before returning. */
    return wait_busy_low(ctx);
}

/* -------------------- sx126x_hal_reset -------------------- */

sx126x_hal_status_t sx126x_hal_reset(const void *context)
{
    (void)context;  /* reset line is on the expander — BSP knows where. */

    if (nesso_lora_reset(true)  != ESP_OK) return SX126X_HAL_STATUS_ERROR;
    vTaskDelay(pdMS_TO_TICKS(2));            /* ≥ 100 µs required, 2 ms is generous */
    if (nesso_lora_reset(false) != ESP_OK) return SX126X_HAL_STATUS_ERROR;

    /* Per SX126x datasheet, give the chip 10 ms to come out of POR before
     * issuing any SPI command. */
    vTaskDelay(pdMS_TO_TICKS(10));
    return SX126X_HAL_STATUS_OK;
}

/* -------------------- sx126x_hal_wakeup -------------------- */

sx126x_hal_status_t sx126x_hal_wakeup(const void *context)
{
    const nesso_sx1262_hal_ctx_t *ctx = (const nesso_sx1262_hal_ctx_t *)context;
    if (!ctx || !ctx->spi) return SX126X_HAL_STATUS_ERROR;

    /*
     * Wake-up pattern: send a 1-byte dummy (GetStatus 0xC0). The falling
     * CS edge pulls the chip out of sleep; BUSY goes high during the
     * wakeup sequence; we then wait for BUSY low.
     */
    const uint8_t get_status = 0xC0;
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &get_status,
        .rx_buffer = NULL,
    };
    if (spi_device_transmit(ctx->spi, &t) != ESP_OK) {
        return SX126X_HAL_STATUS_ERROR;
    }

    return wait_busy_low(ctx);
}
