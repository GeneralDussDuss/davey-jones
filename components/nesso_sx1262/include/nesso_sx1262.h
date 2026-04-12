/*
 * nesso_sx1262 — SX1262 LoRa radio support for the Arduino Nesso N1.
 *
 * Wraps Semtech's reference C driver (vendor/sx126x.*) with Davey Jones
 * plumbing: shared SPI host (nesso_spi), expander-backed reset / antenna
 * switch / LNA enable (nesso_bsp), and a clean init / tx / rx surface.
 *
 * Nesso-specific realities the wrapper handles for you:
 *   - RESET  lives on E0.P7 (NOT a direct GPIO).
 *   - RF switch is on E0.P6 — we toggle it around every TX/RX ourselves,
 *     and do NOT call sx126x_set_dio2_as_rf_sw_ctrl().
 *   - LNA enable is on E0.P5 — we drive it high for RX, low for TX/idle.
 *   - BUSY and DIO1 ARE direct GPIOs (19 and 15).
 *   - SPI bus is shared with the LCD; nesso_spi arbitrates.
 *
 * NOT HANDLED YET (flagged in-code):
 *   - TCXO on DIO3: the Nesso's datasheet doesn't conclusively state
 *     whether the SX1262 module has an internal TCXO powered by DIO3.
 *     Default config uses XTAL mode (no DIO3 TCXO control). Set
 *     tcxo_enable=true in the config only after confirming from schematic.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------- configuration -------------------- */

typedef enum {
    NESSO_LORA_SF5 = 5,
    NESSO_LORA_SF6,
    NESSO_LORA_SF7,
    NESSO_LORA_SF8,
    NESSO_LORA_SF9,
    NESSO_LORA_SF10,
    NESSO_LORA_SF11,
    NESSO_LORA_SF12,
} nesso_lora_sf_t;

typedef enum {
    NESSO_LORA_BW_125 = 0,
    NESSO_LORA_BW_250,
    NESSO_LORA_BW_500,
    NESSO_LORA_BW_62,     /* 62.5 kHz */
    NESSO_LORA_BW_41,     /* 41.7 kHz */
    NESSO_LORA_BW_31,     /* 31.25 kHz */
    NESSO_LORA_BW_20,     /* 20.8 kHz */
    NESSO_LORA_BW_15,     /* 15.6 kHz */
    NESSO_LORA_BW_10,     /* 10.4 kHz */
    NESSO_LORA_BW_7,      /* 7.8 kHz */
} nesso_lora_bw_t;

typedef enum {
    NESSO_LORA_CR_4_5 = 0,
    NESSO_LORA_CR_4_6,
    NESSO_LORA_CR_4_7,
    NESSO_LORA_CR_4_8,
} nesso_lora_cr_t;

typedef struct {
    uint32_t        freq_hz;          /* RF frequency in Hz (e.g. 868000000 or 915000000) */
    int8_t          tx_power_dbm;     /* -9..+22 dBm for SX1262 */
    nesso_lora_sf_t sf;               /* default SF7  */
    nesso_lora_bw_t bw;               /* default 125 kHz */
    nesso_lora_cr_t cr;               /* default 4/5 */
    uint16_t        preamble_len;     /* default 8 symbols */
    uint8_t         sync_word;        /* SX126x shorthand: 0x12 = private LoRa, 0x34 = public LoRaWAN */
    bool            crc_enable;
    bool            invert_iq;        /* downlink-style IQ inversion */

    /* TCXO — leave false unless you've confirmed the Nesso's module wires
     * the TCXO power/control to DIO3. Wrong setting won't damage anything
     * but the radio will silently fail to calibrate. */
    bool     tcxo_enable;
    float    tcxo_voltage;            /* 1.6 / 1.7 / 1.8 / 2.2 / 2.4 / 2.7 / 3.0 / 3.3 */
    uint32_t tcxo_startup_ms;         /* typical 5 ms */
} nesso_sx1262_config_t;

/**
 * Sensible defaults for EU868 / US915 sniffing and point-to-point recon:
 *   SF7 / 125 kHz / 4/5 CR / 8-symbol preamble / public sync word / CRC on.
 * Caller still has to set freq_hz and tx_power_dbm.
 */
#define NESSO_SX1262_CONFIG_DEFAULTS() ((nesso_sx1262_config_t){ \
    .freq_hz        = 868000000,            \
    .tx_power_dbm   = 14,                   \
    .sf             = NESSO_LORA_SF7,       \
    .bw             = NESSO_LORA_BW_125,    \
    .cr             = NESSO_LORA_CR_4_5,    \
    .preamble_len   = 8,                    \
    .sync_word      = 0x34,                 \
    .crc_enable     = true,                 \
    .invert_iq      = false,                \
    .tcxo_enable    = false,                \
    .tcxo_voltage   = 1.8f,                 \
    .tcxo_startup_ms= 5,                    \
})

/* -------------------- lifecycle -------------------- */

/**
 * Bring up the SX1262. Assumes nesso_bsp_init() and nesso_spi_init() have
 * already run. Performs:
 *   1. SPI device attach.
 *   2. GPIO config for BUSY (input) and DIO1 (input with interrupt).
 *   3. Hardware reset via the expander.
 *   4. Standby -> RC.
 *   5. Optional TCXO config + re-calibration.
 *   6. Packet type = LoRa, freq, PA config, TX power, LoRa mod+pkt params.
 *   7. DIO1 ISR install, event queue ready.
 */
esp_err_t nesso_sx1262_init(const nesso_sx1262_config_t *cfg);

/** Power-down, SPI detach, ISR remove. */
esp_err_t nesso_sx1262_deinit(void);

/* -------------------- on-the-fly reconfig -------------------- */

esp_err_t nesso_sx1262_set_freq    (uint32_t freq_hz);
esp_err_t nesso_sx1262_set_tx_power(int8_t dbm);
esp_err_t nesso_sx1262_set_sf_bw_cr(nesso_lora_sf_t sf, nesso_lora_bw_t bw, nesso_lora_cr_t cr);

/* -------------------- TX / RX -------------------- */

/**
 * Send a LoRa packet. Blocks until TX_DONE or timeout.
 * Drives antenna switch + LNA appropriately.
 *
 * @param payload      Bytes to transmit (max 255).
 * @param length       Number of bytes.
 * @param timeout_ms   0 = block forever on TX_DONE; otherwise SX1262 hardware timer.
 */
esp_err_t nesso_sx1262_tx(const uint8_t *payload, uint8_t length, uint32_t timeout_ms);

typedef struct {
    uint8_t  length;
    int8_t   rssi_dbm;
    int8_t   snr_db;
    int8_t   rssi_signal_dbm;
    bool     crc_ok;
} nesso_sx1262_rx_info_t;

/**
 * Receive one LoRa packet.
 * Puts the radio in RX single-mode (or continuous if timeout_ms == 0),
 * blocks until RX_DONE / CRC_ERROR / TIMEOUT.
 *
 * @param out_buf      Destination buffer (≥ 255 bytes recommended).
 * @param out_buf_size Size of out_buf.
 * @param timeout_ms   0 = continuous, otherwise single-shot with HW timer.
 * @param out_info     Populated on success with length/rssi/snr/crc_ok.
 *
 * Returns ESP_OK on a CRC-OK packet, ESP_ERR_INVALID_CRC on CRC error,
 * ESP_ERR_TIMEOUT on HW timeout.
 */
esp_err_t nesso_sx1262_rx(uint8_t *out_buf, size_t out_buf_size,
                          uint32_t timeout_ms,
                          nesso_sx1262_rx_info_t *out_info);

/**
 * Enter RX continuous mode without blocking. DIO1 events land on the
 * queue returned by nesso_sx1262_event_queue(); the caller drains them
 * and uses nesso_sx1262_read_rx_buffer() to fetch payload.
 */
esp_err_t nesso_sx1262_start_rx_continuous(void);

/** Abort any RX/TX and drop to standby. */
esp_err_t nesso_sx1262_standby(void);

/* -------------------- async events -------------------- */

typedef enum {
    NESSO_SX1262_EVT_TX_DONE = 0,
    NESSO_SX1262_EVT_RX_DONE,
    NESSO_SX1262_EVT_CRC_ERROR,
    NESSO_SX1262_EVT_TIMEOUT,
    NESSO_SX1262_EVT_PREAMBLE,
    NESSO_SX1262_EVT_HEADER_VALID,
    NESSO_SX1262_EVT_HEADER_ERROR,
} nesso_sx1262_event_t;

/** FreeRTOS queue the user can xQueueReceive(nesso_sx1262_event_t) from. */
QueueHandle_t nesso_sx1262_event_queue(void);

/** Raw HAL context for direct sx126x driver access (used by nesso_subghz). */
const void *nesso_sx1262_get_hal_ctx(void);

/** Helper for continuous-RX mode: reads latched RX metadata + buffer. */
esp_err_t nesso_sx1262_read_last_rx(uint8_t *out_buf, size_t out_buf_size,
                                    nesso_sx1262_rx_info_t *out_info);

#ifdef __cplusplus
}
#endif
