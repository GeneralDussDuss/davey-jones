/*
 * nesso_sx1262.c — High-level SX1262 wrapper for DAVEY JONES.
 *
 * Sits on top of Semtech's sx126x driver (vendor/sx126x.*) and on top of
 * our nesso_spi / nesso_bsp layers. Hides all the "antenna switch goes on
 * the expander, LNA goes on the expander, reset goes on the expander,
 * BUSY/DIO1 are direct GPIOs, SPI is shared with the LCD" gnarl behind
 * a clean init / tx / rx API.
 */

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nesso_bsp.h"
#include "nesso_spi.h"
#include "nesso_sx1262.h"
#include "nesso_sx1262_hal.h"

/* Semtech reference driver */
#include "sx126x.h"
#include "sx126x_hal.h"
#include "sx126x_status.h"

static const char *TAG = "nesso_sx1262";

/* -------------------- module state -------------------- */

static nesso_sx1262_hal_ctx_t s_hal_ctx = {0};
static SemaphoreHandle_t      s_lock    = NULL;  /* serializes wrapper-level ops */
static QueueHandle_t          s_events  = NULL;  /* DIO1 ISR -> user task */
static nesso_sx1262_config_t  s_cfg;
static bool                   s_ready   = false;

/* Scratch for reading back last RX packet. */
static uint8_t                s_last_rx_buf[255];
static nesso_sx1262_rx_info_t s_last_rx_info;

/* -------------------- small helpers -------------------- */

static inline esp_err_t chk(sx126x_status_t s, const char *where)
{
    if (s == SX126X_STATUS_OK) return ESP_OK;
    ESP_LOGE(TAG, "sx126x op '%s' failed: status=%d", where, (int)s);
    return ESP_FAIL;
}

#define SX_CHK(expr, where) do {                                      \
    esp_err_t __e = chk((expr), (where));                             \
    if (__e != ESP_OK) return __e;                                    \
} while (0)

static sx126x_lora_sf_t map_sf(nesso_lora_sf_t sf)
{
    /* Our enum numerically matches SF5..SF12, and Semtech's uses the
     * same 5..12 values, so a direct cast is safe. */
    return (sx126x_lora_sf_t)sf;
}

static sx126x_lora_bw_t map_bw(nesso_lora_bw_t bw)
{
    switch (bw) {
    case NESSO_LORA_BW_125: return SX126X_LORA_BW_125;
    case NESSO_LORA_BW_250: return SX126X_LORA_BW_250;
    case NESSO_LORA_BW_500: return SX126X_LORA_BW_500;
    case NESSO_LORA_BW_62:  return SX126X_LORA_BW_062;
    case NESSO_LORA_BW_41:  return SX126X_LORA_BW_041;
    case NESSO_LORA_BW_31:  return SX126X_LORA_BW_031;
    case NESSO_LORA_BW_20:  return SX126X_LORA_BW_020;
    case NESSO_LORA_BW_15:  return SX126X_LORA_BW_015;
    case NESSO_LORA_BW_10:  return SX126X_LORA_BW_010;
    case NESSO_LORA_BW_7:   return SX126X_LORA_BW_007;
    }
    return SX126X_LORA_BW_125;
}

static sx126x_lora_cr_t map_cr(nesso_lora_cr_t cr)
{
    switch (cr) {
    case NESSO_LORA_CR_4_5: return SX126X_LORA_CR_4_5;
    case NESSO_LORA_CR_4_6: return SX126X_LORA_CR_4_6;
    case NESSO_LORA_CR_4_7: return SX126X_LORA_CR_4_7;
    case NESSO_LORA_CR_4_8: return SX126X_LORA_CR_4_8;
    }
    return SX126X_LORA_CR_4_5;
}

static sx126x_tcxo_ctrl_voltages_t map_tcxo_voltage(float v)
{
    if (v < 1.65f) return SX126X_TCXO_CTRL_1_6V;
    if (v < 1.75f) return SX126X_TCXO_CTRL_1_7V;
    if (v < 2.00f) return SX126X_TCXO_CTRL_1_8V;
    if (v < 2.30f) return SX126X_TCXO_CTRL_2_2V;
    if (v < 2.55f) return SX126X_TCXO_CTRL_2_4V;
    if (v < 2.85f) return SX126X_TCXO_CTRL_2_7V;
    if (v < 3.15f) return SX126X_TCXO_CTRL_3_0V;
    return SX126X_TCXO_CTRL_3_3V;
}

/* Convert a TCXO startup time in ms to the SX1262's 15.625 µs timer unit. */
static uint32_t tcxo_delay_rtc_steps(uint32_t ms)
{
    /* 1 ms = 64 RTC steps (each 15.625 µs). */
    return ms * 64u;
}

/* -------------------- DIO1 ISR -------------------- */

static void IRAM_ATTR dio1_isr(void *arg)
{
    (void)arg;
    if (!s_events) return;
    /* We don't know which IRQ flags fired here — the ISR can't do I²C/SPI.
     * Post a generic wake; the task that reads the event queue will call
     * sx126x_get_irq_status() to decode. */
    BaseType_t higher = pdFALSE;
    const uint32_t raw = 0xFFFFFFFFu;  /* sentinel: "check status register" */
    xQueueSendFromISR(s_events, &raw, &higher);
    if (higher) portYIELD_FROM_ISR();
}

/*
 * Internal helper: block until DIO1 fires or timeout, then read and clear
 * the IRQ status register. Returns the decoded mask.
 */
static esp_err_t wait_irq(uint32_t timeout_ms, sx126x_irq_mask_t *out_mask)
{
    uint32_t dummy;
    TickType_t ticks = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    if (xQueueReceive(s_events, &dummy, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    sx126x_irq_mask_t flags = 0;
    SX_CHK(sx126x_get_irq_status  (&s_hal_ctx, &flags), "get_irq_status");
    SX_CHK(sx126x_clear_irq_status(&s_hal_ctx, flags),  "clear_irq_status");
    if (out_mask) *out_mask = flags;
    return ESP_OK;
}

/* -------------------- init -------------------- */

static esp_err_t configure_gpios(void)
{
    /* BUSY: plain input, no pull. */
    gpio_config_t busy_cfg = {
        .pin_bit_mask = 1ULL << NESSO_GPIO_LORA_BUSY,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy_cfg), TAG, "busy cfg");

    /* DIO1: rising-edge interrupt. Chip drives it high on any enabled IRQ. */
    gpio_config_t dio1_cfg = {
        .pin_bit_mask = 1ULL << NESSO_GPIO_LORA_DIO1,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&dio1_cfg), TAG, "dio1 cfg");

    /* Install the ISR service if nobody else did. Ignore "already installed". */
    esp_err_t isr_svc = gpio_install_isr_service(0);
    if (isr_svc != ESP_OK && isr_svc != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr_svc));
        return isr_svc;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(NESSO_GPIO_LORA_DIO1, dio1_isr, NULL),
                        TAG, "dio1 isr add");
    return ESP_OK;
}

/*
 * Unwind everything init() has allocated so far. Safe to call with
 * s_ready=false on any partial state. Matches every resource creation
 * in nesso_sx1262_init().
 */
static void sx1262_cleanup(void)
{
    /* GPIO ISR handler first — it could fire during teardown otherwise. */
    gpio_isr_handler_remove(NESSO_GPIO_LORA_DIO1);

    if (s_hal_ctx.spi) {
        nesso_spi_remove(s_hal_ctx.spi);
        s_hal_ctx.spi = NULL;
    }
    if (s_events) { vQueueDelete(s_events);   s_events = NULL; }
    if (s_lock)   { vSemaphoreDelete(s_lock); s_lock   = NULL; }
}

/* Local throw-wrapper: log, stash err, jump to fail. */
#define SX_TRY(expr, where)                                                   \
    do {                                                                      \
        err = chk((expr), (where));                                           \
        if (err != ESP_OK) goto fail;                                         \
    } while (0)

#define ESP_TRY(expr, where)                                                  \
    do {                                                                      \
        err = (expr);                                                         \
        if (err != ESP_OK) {                                                  \
            ESP_LOGE(TAG, "%s: %s", (where), esp_err_to_name(err));           \
            goto fail;                                                        \
        }                                                                     \
    } while (0)

esp_err_t nesso_sx1262_init(const nesso_sx1262_config_t *cfg)
{
    if (s_ready) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;

    esp_err_t err = ESP_OK;

    /* 1. Sync primitives. Allocate one at a time so we can unwind cleanly
     *    if the second allocation fails. */
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "OOM on mutex");
        return ESP_ERR_NO_MEM;
    }
    s_events = xQueueCreate(8, sizeof(uint32_t));
    if (!s_events) {
        ESP_LOGE(TAG, "OOM on event queue");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 2. Attach to the shared SPI bus. SX1262 tops out at 16 MHz; 8 MHz
     *    is a safe default with the Nesso's shared traces. */
    ESP_TRY(nesso_spi_add_lora(8 * 1000 * 1000, &s_hal_ctx.spi), "spi add lora");
    s_hal_ctx.busy_gpio = NESSO_GPIO_LORA_BUSY;
    s_hal_ctx.dio1_gpio = NESSO_GPIO_LORA_DIO1;

    /* 3. Direct GPIOs + DIO1 ISR. */
    ESP_TRY(configure_gpios(), "gpio cfg");

    /* 4. Hardware reset via the expander. nesso_bsp already has RF switch
     *    enable and LNA both driven HIGH from its init — those are static
     *    per the Arduino reference code, not per-operation toggles. */
    SX_TRY(sx126x_reset(&s_hal_ctx), "reset");

    /* 5. Standby to RC oscillator so config writes are accepted. */
    SX_TRY(sx126x_set_standby(&s_hal_ctx, SX126X_STANDBY_CFG_RC), "standby rc");

    /* 5a. Tell the chip to drive DIO2 as its internal RF switch — on the
     *     Nesso, DIO2 toggles TX/RX routing through the external switch
     *     IC that we've already powered on via E0.P6. */
    SX_TRY(sx126x_set_dio2_as_rf_sw_ctrl(&s_hal_ctx, true), "dio2 rf sw ctrl");

    /* 6. Optional TCXO. If the Nesso's SX1262 module has an internal TCXO
     *    wired to DIO3, enable it + re-run all calibrations. Otherwise the
     *    chip uses the 32 MHz XTAL that ships on the module and we skip. */
    if (s_cfg.tcxo_enable) {
        sx126x_tcxo_ctrl_voltages_t v = map_tcxo_voltage(s_cfg.tcxo_voltage);
        uint32_t delay = tcxo_delay_rtc_steps(s_cfg.tcxo_startup_ms ?: 5);
        SX_TRY(sx126x_set_dio3_as_tcxo_ctrl(&s_hal_ctx, v, delay), "tcxo ctrl");
        /* Chip must see the TCXO as its reference before calibration
         * numbers are valid; re-run the whole cal sweep. */
        SX_TRY(sx126x_cal(&s_hal_ctx, SX126X_CAL_ALL), "cal all");
    }

    /* 7. DC-DC for lower TX current. Module has the inductor. */
    SX_TRY(sx126x_set_reg_mode(&s_hal_ctx, SX126X_REG_MODE_DCDC), "reg mode dcdc");

    /* 8. Packet type = LoRa. */
    SX_TRY(sx126x_set_pkt_type(&s_hal_ctx, SX126X_PKT_TYPE_LORA), "pkt type");

    /* 9. RF frequency + image calibration for the band. */
    SX_TRY(sx126x_set_rf_freq(&s_hal_ctx, s_cfg.freq_hz), "set rf freq");
    {
        uint16_t mhz = s_cfg.freq_hz / 1000000u;
        SX_TRY(sx126x_cal_img_in_mhz(&s_hal_ctx, mhz, mhz), "cal img");
    }

    /* 10. PA config. Datasheet §13.1.14 table: for SX1262 at +22 dBm use
     *     pa_duty_cycle=0x04, hp_max=0x07, device_sel=0x00, pa_lut=0x01. */
    const sx126x_pa_cfg_params_t pa = {
        .pa_duty_cycle = 0x04,
        .hp_max        = 0x07,
        .device_sel    = 0x00,
        .pa_lut        = 0x01,
    };
    SX_TRY(sx126x_set_pa_cfg(&s_hal_ctx, &pa), "pa cfg");
    SX_TRY(sx126x_set_tx_params(&s_hal_ctx, s_cfg.tx_power_dbm, SX126X_RAMP_40_US),
           "tx params");

    /* 11. Buffer base addresses (TX at 0, RX at 0). */
    SX_TRY(sx126x_set_buffer_base_address(&s_hal_ctx, 0x00, 0x00), "buf base");

    /* 12. LoRa modulation + packet params. */
    const sx126x_mod_params_lora_t mp = {
        .sf   = map_sf(s_cfg.sf),
        .bw   = map_bw(s_cfg.bw),
        .cr   = map_cr(s_cfg.cr),
        .ldro = 0,  /* Low data rate optimization — only set for SF11/12 @ 125 kHz. TODO */
    };
    SX_TRY(sx126x_set_lora_mod_params(&s_hal_ctx, &mp), "lora mod");

    const sx126x_pkt_params_lora_t pp = {
        .preamble_len_in_symb = s_cfg.preamble_len,
        .header_type          = SX126X_LORA_PKT_EXPLICIT,
        .pld_len_in_bytes     = 0,  /* updated per-TX in nesso_sx1262_tx() */
        .crc_is_on            = s_cfg.crc_enable,
        .invert_iq_is_on      = s_cfg.invert_iq,
    };
    SX_TRY(sx126x_set_lora_pkt_params(&s_hal_ctx, &pp), "lora pkt");

    /* 13. Sync word (private vs public). */
    SX_TRY(sx126x_set_lora_sync_word(&s_hal_ctx, s_cfg.sync_word), "sync word");

    /* 14. IRQ routing: everything we care about goes to DIO1. */
    const uint16_t irq =
        SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE | SX126X_IRQ_CRC_ERROR |
        SX126X_IRQ_TIMEOUT | SX126X_IRQ_HEADER_VALID | SX126X_IRQ_HEADER_ERROR |
        SX126X_IRQ_PREAMBLE_DETECTED;
    SX_TRY(sx126x_set_dio_irq_params(&s_hal_ctx, irq, irq, 0, 0), "dio irq");

    s_ready = true;
    ESP_LOGI(TAG, "SX1262 up @ %lu Hz, power=%d dBm, SF=%d BW=%d CR=%d",
             (unsigned long)s_cfg.freq_hz,
             (int)s_cfg.tx_power_dbm,
             (int)s_cfg.sf, (int)s_cfg.bw, (int)s_cfg.cr);
    return ESP_OK;

fail:
    sx1262_cleanup();
    return err;
}

#undef SX_TRY
#undef ESP_TRY

esp_err_t nesso_sx1262_deinit(void)
{
    if (!s_ready) return ESP_OK;

    gpio_isr_handler_remove(NESSO_GPIO_LORA_DIO1);
    sx126x_set_standby(&s_hal_ctx, SX126X_STANDBY_CFG_RC);
    /* Power-down: drop LNA + RF switch power, then hold in reset. */
    nesso_lora_lna(false);
    nesso_lora_rf_switch(false);
    nesso_lora_reset(true);

    if (s_hal_ctx.spi) { nesso_spi_remove(s_hal_ctx.spi); s_hal_ctx.spi = NULL; }
    if (s_lock)   { vSemaphoreDelete(s_lock);   s_lock   = NULL; }
    if (s_events) { vQueueDelete(s_events);     s_events = NULL; }
    s_ready = false;
    return ESP_OK;
}

/* -------------------- on-the-fly reconfig -------------------- */

esp_err_t nesso_sx1262_set_freq(uint32_t freq_hz)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = chk(sx126x_set_standby(&s_hal_ctx, SX126X_STANDBY_CFG_RC), "standby");
    if (err == ESP_OK) {
        err = chk(sx126x_set_rf_freq(&s_hal_ctx, freq_hz), "rf freq");
    }
    if (err == ESP_OK) {
        uint16_t mhz = freq_hz / 1000000u;
        err = chk(sx126x_cal_img_in_mhz(&s_hal_ctx, mhz, mhz), "cal img");
    }
    if (err == ESP_OK) s_cfg.freq_hz = freq_hz;
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

esp_err_t nesso_sx1262_set_tx_power(int8_t dbm)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = chk(sx126x_set_tx_params(&s_hal_ctx, dbm, SX126X_RAMP_40_US),
                        "tx params");
    if (err == ESP_OK) s_cfg.tx_power_dbm = dbm;
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

esp_err_t nesso_sx1262_set_sf_bw_cr(nesso_lora_sf_t sf, nesso_lora_bw_t bw, nesso_lora_cr_t cr)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    const sx126x_mod_params_lora_t mp = {
        .sf   = map_sf(sf),
        .bw   = map_bw(bw),
        .cr   = map_cr(cr),
        .ldro = 0,
    };
    esp_err_t err = chk(sx126x_set_lora_mod_params(&s_hal_ctx, &mp), "lora mod");
    if (err == ESP_OK) { s_cfg.sf = sf; s_cfg.bw = bw; s_cfg.cr = cr; }
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

/* -------------------- TX -------------------- */

esp_err_t nesso_sx1262_tx(const uint8_t *payload, uint8_t length, uint32_t timeout_ms)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!payload || length == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;

    /* Update packet length + write payload. */
    const sx126x_pkt_params_lora_t pp = {
        .preamble_len_in_symb = s_cfg.preamble_len,
        .header_type          = SX126X_LORA_PKT_EXPLICIT,
        .pld_len_in_bytes     = length,
        .crc_is_on            = s_cfg.crc_enable,
        .invert_iq_is_on      = s_cfg.invert_iq,
    };
    err = chk(sx126x_set_lora_pkt_params(&s_hal_ctx, &pp), "lora pkt (tx)");
    if (err != ESP_OK) goto out;

    err = chk(sx126x_write_buffer(&s_hal_ctx, 0x00, payload, length), "write buf");
    if (err != ESP_OK) goto out;

    /* Drain any stale IRQ events so wait_irq sees only this TX. */
    xQueueReset(s_events);

    /* No antenna switch / LNA toggle — DIO2 handles routing. */

    /* Kick off TX. Datasheet: 0 = no timeout, otherwise HW timer in ms. */
    err = chk(sx126x_set_tx(&s_hal_ctx, timeout_ms), "set tx");
    if (err != ESP_OK) goto out;

    /* Wait for TX_DONE (or TIMEOUT). wait_irq returns ESP_ERR_TIMEOUT on
     * host-side timeout — in that case we also force a standby. */
    sx126x_irq_mask_t flags = 0;
    const uint32_t host_timeout = timeout_ms ? timeout_ms + 500 : 0;
    err = wait_irq(host_timeout, &flags);
    if (err == ESP_OK) {
        if (flags & SX126X_IRQ_TX_DONE) {
            err = ESP_OK;
        } else if (flags & SX126X_IRQ_TIMEOUT) {
            err = ESP_ERR_TIMEOUT;
        } else {
            ESP_LOGW(TAG, "TX ended with unexpected flags 0x%04x", flags);
            err = ESP_FAIL;
        }
    }

out:
    /* Always restore idle state: standby. Antenna switch + LNA stay
     * enabled per static BSP config. */
    sx126x_set_standby(&s_hal_ctx, SX126X_STANDBY_CFG_RC);
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

/* -------------------- RX -------------------- */

static esp_err_t finalize_rx(uint8_t *out_buf, size_t out_buf_size,
                             nesso_sx1262_rx_info_t *out_info, bool crc_ok)
{
    sx126x_rx_buffer_status_t bstat = {0};
    SX_CHK(sx126x_get_rx_buffer_status(&s_hal_ctx, &bstat), "get rx buf status");

    sx126x_pkt_status_lora_t pstat = {0};
    SX_CHK(sx126x_get_lora_pkt_status(&s_hal_ctx, &pstat), "get lora pkt status");

    uint8_t len = bstat.pld_len_in_bytes;
    if (len > out_buf_size) len = out_buf_size;
    if (out_buf && len) {
        SX_CHK(sx126x_read_buffer(&s_hal_ctx, bstat.buffer_start_pointer,
                                  out_buf, len), "read buf");
    }

    /* Stash for nesso_sx1262_read_last_rx(). */
    uint8_t stash_len = (len > sizeof(s_last_rx_buf)) ? sizeof(s_last_rx_buf) : len;
    if (stash_len && out_buf) memcpy(s_last_rx_buf, out_buf, stash_len);

    s_last_rx_info.length          = len;
    s_last_rx_info.rssi_dbm        = pstat.rssi_pkt_in_dbm;
    s_last_rx_info.snr_db          = pstat.snr_pkt_in_db;
    s_last_rx_info.rssi_signal_dbm = pstat.signal_rssi_pkt_in_dbm;
    s_last_rx_info.crc_ok          = crc_ok;

    if (out_info) *out_info = s_last_rx_info;
    return ESP_OK;
}

esp_err_t nesso_sx1262_rx(uint8_t *out_buf, size_t out_buf_size,
                          uint32_t timeout_ms,
                          nesso_sx1262_rx_info_t *out_info)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!out_buf || out_buf_size == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err;

    /* No antenna switch / LNA toggle — DIO2 handles routing, LNA is
     * always enabled from BSP init. */
    xQueueReset(s_events);

    err = chk(sx126x_set_rx(&s_hal_ctx, timeout_ms), "set rx");
    if (err != ESP_OK) goto out;

    sx126x_irq_mask_t flags = 0;
    const uint32_t host_timeout = timeout_ms ? timeout_ms + 500 : 0;
    err = wait_irq(host_timeout, &flags);
    if (err != ESP_OK) goto out;

    if (flags & SX126X_IRQ_TIMEOUT) {
        err = ESP_ERR_TIMEOUT;
    } else if (flags & SX126X_IRQ_CRC_ERROR) {
        (void)finalize_rx(out_buf, out_buf_size, out_info, false);
        err = ESP_ERR_INVALID_CRC;
    } else if (flags & SX126X_IRQ_RX_DONE) {
        err = finalize_rx(out_buf, out_buf_size, out_info, true);
    } else {
        ESP_LOGW(TAG, "RX ended with unexpected flags 0x%04x", flags);
        err = ESP_FAIL;
    }

out:
    sx126x_set_standby(&s_hal_ctx, SX126X_STANDBY_CFG_RC);
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

esp_err_t nesso_sx1262_start_rx_continuous(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    /* 0 timeout = continuous RX. */
    esp_err_t err = chk(sx126x_set_rx(&s_hal_ctx, 0), "set rx continuous");
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

esp_err_t nesso_sx1262_standby(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = chk(sx126x_set_standby(&s_hal_ctx, SX126X_STANDBY_CFG_RC), "standby");
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

/* -------------------- events -------------------- */

QueueHandle_t nesso_sx1262_event_queue(void) { return s_events; }

esp_err_t nesso_sx1262_read_last_rx(uint8_t *out_buf, size_t out_buf_size,
                                    nesso_sx1262_rx_info_t *out_info)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!out_info) return ESP_ERR_INVALID_ARG;
    *out_info = s_last_rx_info;
    if (out_buf && out_buf_size) {
        size_t n = s_last_rx_info.length;
        if (n > out_buf_size) n = out_buf_size;
        memcpy(out_buf, s_last_rx_buf, n);
    }
    return ESP_OK;
}
