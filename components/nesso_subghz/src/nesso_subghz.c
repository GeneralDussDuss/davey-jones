/*
 * nesso_subghz.c — Sub-GHz spectrum analyzer, capture, and replay.
 *
 * Spectrum sweep: rapidly steps through frequencies, reads instantaneous
 * RSSI at each point, builds a 135-point array for LCD display.
 *
 * Capture: monitors RSSI at a target frequency, records raw OOK bit
 * patterns by sampling RSSI at high rate and encoding high/low transitions.
 *
 * Replay: switches to TX mode and transmits the captured bit pattern.
 */

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nesso_subghz.h"
#include "nesso_bsp.h"

/* Semtech driver */
#include "sx126x.h"

/* We use the HAL context from nesso_sx1262 — it's the same radio. */
extern const void *nesso_sx1262_get_hal_ctx(void);

static const char *TAG = "subghz";

/* Band definitions — index matches subghz_band_t.
 * Nesso N1 SX1262 module: 850-960 MHz ONLY. */
static const uint32_t s_band_start[] = { 850000000, 860000000, 902000000 };
static const uint32_t s_band_end[]   = { 960000000, 870000000, 928000000 };

/* -------------------- spectrum analyzer -------------------- */

esp_err_t nesso_subghz_sweep(subghz_band_t band, subghz_spectrum_t *out)
{
    if (!out || band >= SUBGHZ_BAND_COUNT) return ESP_ERR_INVALID_ARG;

    const void *ctx = nesso_sx1262_get_hal_ctx();
    if (!ctx) return ESP_ERR_INVALID_STATE;

    out->band = band;
    out->freq_start_hz = s_band_start[band];
    out->freq_end_hz   = s_band_end[band];
    out->rssi_peak     = -128;
    out->peak_freq_hz  = out->freq_start_hz;

    if (out->freq_end_hz <= out->freq_start_hz) return ESP_ERR_INVALID_ARG;
    uint32_t step = (out->freq_end_hz - out->freq_start_hz) / SUBGHZ_SPECTRUM_POINTS;
    if (step == 0) step = 1;

    /*
     * CRITICAL: the SX1262 frequency register only takes effect when
     * transitioning from standby to RX/TX. You CANNOT change frequency
     * while already in RX — the radio stays on the old frequency.
     *
     * Correct sweep: for each point, go standby → set freq → RX → read RSSI.
     * This is slower (~1ms per point = ~240ms per sweep) but actually works.
     */
    sx126x_set_standby(ctx, SX126X_STANDBY_CFG_RC);

    /* Use GFSK (FSK) mode for the sweep — wider bandwidth than LoRa,
     * gives better RSSI readings for arbitrary signals. */
    sx126x_set_pkt_type(ctx, SX126X_PKT_TYPE_GFSK);

    for (int i = 0; i < SUBGHZ_SPECTRUM_POINTS; ++i) {
        uint32_t freq = out->freq_start_hz + (uint32_t)i * step;

        /* Standby → set freq → RX → dwell → read RSSI → standby. */
        sx126x_set_rf_freq(ctx, freq);
        sx126x_set_rx(ctx, 100);  /* 100ms timeout — we'll read before it expires */

        /* Dwell 2ms: PLL lock (~500µs) + receiver settle (~1ms) + RSSI valid. */
        vTaskDelay(pdMS_TO_TICKS(2));

        /* sx126x_get_rssi_inst takes int16_t*, NOT int8_t*! */
        int16_t rssi16 = -128;
        sx126x_get_rssi_inst(ctx, &rssi16);
        int8_t rssi = (rssi16 < -128) ? -128 : (rssi16 > 0) ? 0 : (int8_t)rssi16;

        out->rssi[i] = rssi;
        if (rssi > out->rssi_peak) {
            out->rssi_peak = rssi;
            out->peak_freq_hz = freq;
        }

        sx126x_set_standby(ctx, SX126X_STANDBY_CFG_RC);
    }

    /* Restore LoRa mode for other functions. */
    sx126x_set_pkt_type(ctx, SX126X_PKT_TYPE_LORA);

    return ESP_OK;
}

/* -------------------- capture -------------------- */

esp_err_t nesso_subghz_capture(uint32_t freq_hz, uint32_t duration_ms,
                                int8_t rssi_threshold,
                                subghz_capture_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    const void *ctx = nesso_sx1262_get_hal_ctx();
    if (!ctx) return ESP_ERR_INVALID_STATE;

    memset(out, 0, sizeof(*out));
    out->freq_hz = freq_hz;

    /* Set frequency and enter RX. */
    sx126x_set_standby(ctx, SX126X_STANDBY_CFG_RC);
    sx126x_set_pkt_type(ctx, SX126X_PKT_TYPE_LORA);
    sx126x_set_rf_freq(ctx, freq_hz);
    sx126x_set_rx(ctx, 0);

    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000ULL);
    size_t byte_idx = 0;
    uint8_t current_byte = 0;
    int bit_pos = 7;
    bool signal_detected = false;

    ESP_LOGI(TAG, "capturing at %lu Hz, threshold %d dBm, %lu ms",
             (unsigned long)freq_hz, rssi_threshold, (unsigned long)duration_ms);

    while (1) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if ((now - start) > duration_ms) break;
        if (byte_idx >= SUBGHZ_CAPTURE_MAX_BYTES) break;

        int16_t rssi16 = -128;
        sx126x_get_rssi_inst(ctx, &rssi16);
        int8_t rssi = (rssi16 < -128) ? -128 : (rssi16 > 0) ? 0 : (int8_t)rssi16;

        /* Threshold-based OOK decoding: above threshold = 1, below = 0. */
        bool bit = (rssi > rssi_threshold);
        if (bit) signal_detected = true;

        if (bit) current_byte |= (1 << bit_pos);
        bit_pos--;
        if (bit_pos < 0) {
            out->data[byte_idx++] = current_byte;
            current_byte = 0;
            bit_pos = 7;
        }

        /* Sample at ~5 kHz (200µs per sample). */
        esp_rom_delay_us(200);
    }

    /* Flush partial byte. */
    if (bit_pos < 7 && byte_idx < SUBGHZ_CAPTURE_MAX_BYTES) {
        out->data[byte_idx++] = current_byte;
    }

    out->length = byte_idx;
    out->duration_ms = (uint32_t)(esp_timer_get_time() / 1000ULL) - start;

    sx126x_set_standby(ctx, SX126X_STANDBY_CFG_RC);

    if (!signal_detected) {
        ESP_LOGW(TAG, "no signal detected above %d dBm", rssi_threshold);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "captured %zu bytes in %lu ms", out->length, (unsigned long)out->duration_ms);
    return ESP_OK;
}

/* -------------------- save / load -------------------- */

esp_err_t nesso_subghz_save(const subghz_capture_t *cap, const char *path)
{
    if (!cap || !path) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    fwrite(&cap->freq_hz, sizeof(cap->freq_hz), 1, f);
    fwrite(&cap->length, sizeof(cap->length), 1, f);
    fwrite(&cap->duration_ms, sizeof(cap->duration_ms), 1, f);
    fwrite(cap->data, 1, cap->length, f);
    fclose(f);
    ESP_LOGI(TAG, "saved %zu bytes to %s", cap->length, path);
    return ESP_OK;
}

esp_err_t nesso_subghz_load(const char *path, subghz_capture_t *out)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    memset(out, 0, sizeof(*out));
    fread(&out->freq_hz, sizeof(out->freq_hz), 1, f);
    fread(&out->length, sizeof(out->length), 1, f);
    fread(&out->duration_ms, sizeof(out->duration_ms), 1, f);
    if (out->length > SUBGHZ_CAPTURE_MAX_BYTES) out->length = SUBGHZ_CAPTURE_MAX_BYTES;
    fread(out->data, 1, out->length, f);
    fclose(f);
    ESP_LOGI(TAG, "loaded %zu bytes from %s @ %lu Hz",
             out->length, path, (unsigned long)out->freq_hz);
    return ESP_OK;
}

/* -------------------- replay -------------------- */

esp_err_t nesso_subghz_replay(const subghz_capture_t *cap)
{
    if (!cap || cap->length == 0) return ESP_ERR_INVALID_ARG;

    const void *ctx = nesso_sx1262_get_hal_ctx();
    if (!ctx) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "replaying %zu bytes at %lu Hz",
             cap->length, (unsigned long)cap->freq_hz);

    /* Set frequency. */
    sx126x_set_standby(ctx, SX126X_STANDBY_CFG_RC);
    sx126x_set_rf_freq(ctx, cap->freq_hz);

    /* Write captured data to the radio buffer and TX.
     * For proper OOK replay, we'd need the radio in OOK TX mode with
     * bit-by-bit timing. As a first pass, we use LoRa packet TX which
     * sends the raw bytes as a LoRa frame. This won't work for OOK
     * devices but proves the pipeline. TODO: implement proper OOK TX. */
    sx126x_set_pkt_type(ctx, SX126X_PKT_TYPE_LORA);
    sx126x_set_buffer_base_address(ctx, 0x00, 0x00);

    uint8_t tx_len = cap->length > 255 ? 255 : (uint8_t)cap->length;
    sx126x_write_buffer(ctx, 0x00, cap->data, tx_len);
    sx126x_set_tx(ctx, 3000);

    /* Wait for TX done. */
    vTaskDelay(pdMS_TO_TICKS(500));

    sx126x_set_standby(ctx, SX126X_STANDBY_CFG_RC);

    ESP_LOGI(TAG, "replay complete");
    return ESP_OK;
}

/* ==================== LoRa Sniffer ==================== */

static lora_sniff_state_t s_lora_sniff = {0};
static TaskHandle_t s_lora_sniff_task = NULL;

static void lora_sniff_task(void *arg)
{
    (void)arg;
    const void *ctx = nesso_sx1262_get_hal_ctx();
    if (!ctx) { s_lora_sniff.running = false; vTaskDelete(NULL); return; }

    /* Put radio in continuous RX. */
    sx126x_set_rx(ctx, 0);
    ESP_LOGI(TAG, "LoRa sniffer running");

    while (s_lora_sniff.running) {
        /* Check for RX done via IRQ status. */
        sx126x_irq_mask_t irq = 0;
        sx126x_get_irq_status(ctx, &irq);

        if (irq & (SX126X_IRQ_RX_DONE | SX126X_IRQ_CRC_ERROR)) {
            sx126x_clear_irq_status(ctx, irq);

            /* Read the received packet. */
            sx126x_rx_buffer_status_t bstat = {0};
            sx126x_get_rx_buffer_status(ctx, &bstat);

            sx126x_pkt_status_lora_t pstat = {0};
            sx126x_get_lora_pkt_status(ctx, &pstat);

            uint8_t len = bstat.pld_len_in_bytes;
            if (len > 64) len = 64;

            s_lora_sniff.total_seen++;

            if (s_lora_sniff.count < LORA_SNIFF_MAX_PKTS) {
                lora_sniff_pkt_t *p = &s_lora_sniff.packets[s_lora_sniff.count++];
                p->length = len;
                p->rssi = pstat.rssi_pkt_in_dbm;
                p->snr = pstat.snr_pkt_in_db;
                p->timestamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);

                if (len > 0) {
                    sx126x_read_buffer(ctx, bstat.buffer_start_pointer, p->data, len);
                }

                ESP_LOGI(TAG, "LoRa pkt: %u bytes RSSI=%d SNR=%d",
                         len, p->rssi, p->snr);
            }

            /* Stay in RX. */
            sx126x_set_rx(ctx, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    sx126x_set_standby(ctx, SX126X_STANDBY_CFG_RC);
    s_lora_sniff_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t nesso_lora_sniff_start(uint32_t freq_hz, uint8_t sf, uint8_t bw)
{
    if (s_lora_sniff.running) return ESP_OK;

    const void *ctx = nesso_sx1262_get_hal_ctx();
    if (!ctx) return ESP_ERR_INVALID_STATE;

    /* Configure radio for the requested parameters. */
    sx126x_set_standby(ctx, SX126X_STANDBY_CFG_RC);
    sx126x_set_pkt_type(ctx, SX126X_PKT_TYPE_LORA);
    sx126x_set_rf_freq(ctx, freq_hz);

    sx126x_mod_params_lora_t mp = {
        .sf = (sx126x_lora_sf_t)sf,
        .bw = (sx126x_lora_bw_t)bw,
        .cr = SX126X_LORA_CR_4_5,
        .ldro = 0,
    };
    sx126x_set_lora_mod_params(ctx, &mp);

    /* Explicit header, max payload, CRC on. */
    sx126x_pkt_params_lora_t pp = {
        .preamble_len_in_symb = 8,
        .header_type = SX126X_LORA_PKT_EXPLICIT,
        .pld_len_in_bytes = 255,
        .crc_is_on = true,
        .invert_iq_is_on = false,
    };
    sx126x_set_lora_pkt_params(ctx, &pp);

    /* IRQ on RX done + CRC error. */
    uint16_t irq = SX126X_IRQ_RX_DONE | SX126X_IRQ_CRC_ERROR;
    sx126x_set_dio_irq_params(ctx, irq, irq, 0, 0);

    memset(&s_lora_sniff, 0, sizeof(s_lora_sniff));
    s_lora_sniff.running = true;

    BaseType_t ok = xTaskCreate(lora_sniff_task, "lora_sniff", 4096, NULL, 5, &s_lora_sniff_task);
    if (ok != pdPASS) { s_lora_sniff.running = false; return ESP_ERR_NO_MEM; }

    ESP_LOGI(TAG, "LoRa sniffer: %lu Hz SF%u BW%u", (unsigned long)freq_hz, sf, bw);
    return ESP_OK;
}

esp_err_t nesso_lora_sniff_stop(void)
{
    if (!s_lora_sniff.running) return ESP_OK;
    s_lora_sniff.running = false;
    for (int i = 0; i < 20 && s_lora_sniff_task; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t nesso_lora_sniff_get(lora_sniff_state_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = s_lora_sniff;
    return ESP_OK;
}

/* ==================== LoRa TX ==================== */

esp_err_t nesso_lora_send(uint32_t freq_hz, const char *msg)
{
    if (!msg) return ESP_ERR_INVALID_ARG;

    const void *ctx = nesso_sx1262_get_hal_ctx();
    if (!ctx) return ESP_ERR_INVALID_STATE;

    uint8_t len = (uint8_t)strlen(msg);
    if (len > 200) len = 200;

    sx126x_set_standby(ctx, SX126X_STANDBY_CFG_RC);
    sx126x_set_pkt_type(ctx, SX126X_PKT_TYPE_LORA);
    sx126x_set_rf_freq(ctx, freq_hz);

    sx126x_pkt_params_lora_t pp = {
        .preamble_len_in_symb = 8,
        .header_type = SX126X_LORA_PKT_EXPLICIT,
        .pld_len_in_bytes = len,
        .crc_is_on = true,
        .invert_iq_is_on = false,
    };
    sx126x_set_lora_pkt_params(ctx, &pp);

    sx126x_set_buffer_base_address(ctx, 0x00, 0x00);
    sx126x_write_buffer(ctx, 0x00, (const uint8_t *)msg, len);
    sx126x_set_tx(ctx, 3000);

    vTaskDelay(pdMS_TO_TICKS(500));
    sx126x_set_standby(ctx, SX126X_STANDBY_CFG_RC);

    ESP_LOGI(TAG, "LoRa TX: %u bytes @ %lu Hz", len, (unsigned long)freq_hz);
    return ESP_OK;
}
