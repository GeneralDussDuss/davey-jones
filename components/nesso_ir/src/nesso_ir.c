/*
 * nesso_ir.c — IR transmitter via RMT on GPIO9.
 */

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nesso_bsp.h"
#include "nesso_ir.h"

static const char *TAG = "nesso_ir";

static rmt_channel_handle_t s_tx_chan = NULL;
static rmt_encoder_handle_t s_encoder = NULL;
static bool s_ready = false;

/* NEC timing (in microseconds). */
#define NEC_LEAD_ON    9000
#define NEC_LEAD_OFF   4500
#define NEC_BIT_ON     560
#define NEC_ONE_OFF    1690
#define NEC_ZERO_OFF   560
#define NEC_STOP_ON    560
#define NEC_CARRIER_HZ 38000

/* Samsung timing — similar to NEC but different lead. */
#define SAM_LEAD_ON    4500
#define SAM_LEAD_OFF   4500

/* -------------------- RMT encoder for IR -------------------- */

/*
 * We use the copy encoder for raw RMT symbols. Build the symbol
 * array manually and transmit.
 */

/* Max symbols: lead(1) + 32 data bits + stop(1) = 34 */
#define MAX_SYMBOLS 36

static rmt_symbol_word_t s_symbols[MAX_SYMBOLS];

static int encode_nec_frame(uint32_t data, rmt_symbol_word_t *syms)
{
    int idx = 0;
    /* Lead pulse. */
    syms[idx].duration0 = NEC_LEAD_ON;  syms[idx].level0 = 1;
    syms[idx].duration1 = NEC_LEAD_OFF; syms[idx].level1 = 0;
    idx++;

    /* 32 data bits, MSB first. */
    for (int i = 31; i >= 0; --i) {
        syms[idx].duration0 = NEC_BIT_ON;  syms[idx].level0 = 1;
        if (data & (1UL << i)) {
            syms[idx].duration1 = NEC_ONE_OFF; syms[idx].level1 = 0;
        } else {
            syms[idx].duration1 = NEC_ZERO_OFF; syms[idx].level1 = 0;
        }
        idx++;
    }

    /* Stop bit. */
    syms[idx].duration0 = NEC_STOP_ON; syms[idx].level0 = 1;
    syms[idx].duration1 = 0;          syms[idx].level1 = 0;
    idx++;

    return idx;
}

static int encode_samsung_frame(uint32_t data, rmt_symbol_word_t *syms)
{
    int idx = 0;
    syms[idx].duration0 = SAM_LEAD_ON;  syms[idx].level0 = 1;
    syms[idx].duration1 = SAM_LEAD_OFF; syms[idx].level1 = 0;
    idx++;

    for (int i = 31; i >= 0; --i) {
        syms[idx].duration0 = NEC_BIT_ON; syms[idx].level0 = 1;
        if (data & (1UL << i)) {
            syms[idx].duration1 = NEC_ONE_OFF; syms[idx].level1 = 0;
        } else {
            syms[idx].duration1 = NEC_ZERO_OFF; syms[idx].level1 = 0;
        }
        idx++;
    }

    syms[idx].duration0 = NEC_STOP_ON; syms[idx].level0 = 1;
    syms[idx].duration1 = 0;          syms[idx].level1 = 0;
    idx++;

    return idx;
}

/* Build the 32-bit NEC data word from address + command. */
static uint32_t nec_data(uint16_t addr, uint8_t cmd)
{
    /* Standard NEC: addr(8) + ~addr(8) + cmd(8) + ~cmd(8)
     * Extended NEC: addr_lo(8) + addr_hi(8) + cmd(8) + ~cmd(8) */
    uint8_t addr_lo = addr & 0xFF;
    uint8_t addr_hi = (addr > 0xFF) ? ((addr >> 8) & 0xFF) : (uint8_t)~addr_lo;
    return ((uint32_t)addr_lo << 24) | ((uint32_t)addr_hi << 16) |
           ((uint32_t)cmd << 8) | (uint32_t)(~cmd & 0xFF);
}

static uint32_t samsung_data(uint16_t addr, uint8_t cmd)
{
    uint8_t addr_lo = addr & 0xFF;
    uint8_t addr_hi = (addr >> 8) & 0xFF;
    return ((uint32_t)addr_lo << 24) | ((uint32_t)addr_hi << 16) |
           ((uint32_t)cmd << 8) | (uint32_t)(~cmd & 0xFF);
}

/* -------------------- lifecycle -------------------- */

esp_err_t nesso_ir_init(void)
{
    if (s_ready) return ESP_OK;

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num           = NESSO_GPIO_IR_TX,
        .clk_src            = RMT_CLK_SRC_DEFAULT,
        .resolution_hz      = 1000000,  /* 1 MHz = 1 µs per tick */
        .mem_block_symbols  = 64,
        .trans_queue_depth  = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &s_tx_chan), TAG, "tx channel");

    /* Carrier for 38 kHz modulation. */
    rmt_carrier_config_t carrier = {
        .frequency_hz       = NEC_CARRIER_HZ,
        .duty_cycle         = 0.33f,
    };
    ESP_RETURN_ON_ERROR(rmt_apply_carrier(s_tx_chan, &carrier), TAG, "carrier");

    /* Copy encoder — we feed pre-built symbol arrays. */
    rmt_copy_encoder_config_t enc_cfg = {};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&enc_cfg, &s_encoder), TAG, "encoder");

    ESP_RETURN_ON_ERROR(rmt_enable(s_tx_chan), TAG, "enable");

    s_ready = true;
    ESP_LOGI(TAG, "IR TX ready on GPIO%d", NESSO_GPIO_IR_TX);
    return ESP_OK;
}

esp_err_t nesso_ir_deinit(void)
{
    if (!s_ready) return ESP_OK;
    if (s_tx_chan) { rmt_disable(s_tx_chan); rmt_del_channel(s_tx_chan); s_tx_chan = NULL; }
    if (s_encoder) { rmt_del_encoder(s_encoder); s_encoder = NULL; }
    s_ready = false;
    return ESP_OK;
}

bool nesso_ir_is_ready(void) { return s_ready; }

/* -------------------- transmit -------------------- */

static esp_err_t send_symbols(const rmt_symbol_word_t *syms, int count)
{
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    return rmt_transmit(s_tx_chan, s_encoder, syms, count * sizeof(rmt_symbol_word_t), &tx_cfg);
}

esp_err_t nesso_ir_send_nec(uint16_t address, uint8_t command)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    uint32_t data = nec_data(address, command);
    int n = encode_nec_frame(data, s_symbols);
    esp_err_t err = send_symbols(s_symbols, n);
    if (err == ESP_OK) {
        rmt_tx_wait_all_done(s_tx_chan, 200);
    }
    return err;
}

esp_err_t nesso_ir_send_samsung(uint16_t address, uint8_t command)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    uint32_t data = samsung_data(address, command);
    int n = encode_samsung_frame(data, s_symbols);
    esp_err_t err = send_symbols(s_symbols, n);
    if (err == ESP_OK) {
        rmt_tx_wait_all_done(s_tx_chan, 200);
    }
    return err;
}

/* -------------------- TV-B-Gone -------------------- */

typedef struct {
    uint16_t addr;
    uint8_t  cmd;
    bool     samsung;  /* true = Samsung protocol, false = NEC */
} tvbgone_code_t;

/* Common TV power-off codes. */
static const tvbgone_code_t s_codes[] = {
    /* Samsung */
    { 0x0707, 0x02, true },
    { 0x0707, 0x98, true },
    /* LG (NEC) */
    { 0x04, 0x08, false },
    { 0x04, 0xC4, false },
    /* Sony (NEC-like — not exact but close enough to hit some) */
    { 0x01, 0x15, false },
    { 0x01, 0x95, false },
    /* Panasonic (NEC extended) */
    { 0x4004, 0x00, false },
    { 0x4004, 0x40, false },
    /* Toshiba */
    { 0x40, 0x12, false },
    { 0x02FD, 0x48, false },
    /* Vizio */
    { 0x04, 0x08, false },
    { 0x0F, 0x12, false },
    /* Philips (NEC) */
    { 0x08, 0x0C, false },
    /* Hisense */
    { 0x00, 0x08, false },
    /* TCL/Roku */
    { 0x04, 0x02, false },
    /* Sharp */
    { 0x1CE0, 0x48, false },
    /* Generic power toggles */
    { 0x00, 0x40, false },
    { 0x00, 0x0C, false },
    { 0xFF, 0x00, false },
    { 0x01, 0x01, false },
};
#define TVBGONE_COUNT (sizeof(s_codes) / sizeof(s_codes[0]))

int nesso_ir_tvbgone(void)
{
    if (!s_ready) return 0;
    int sent = 0;
    for (size_t i = 0; i < TVBGONE_COUNT; ++i) {
        esp_err_t err;
        if (s_codes[i].samsung) {
            err = nesso_ir_send_samsung(s_codes[i].addr, s_codes[i].cmd);
        } else {
            err = nesso_ir_send_nec(s_codes[i].addr, s_codes[i].cmd);
        }
        if (err == ESP_OK) sent++;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return sent;
}
