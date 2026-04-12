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

/* Band definitions. */
static const uint32_t s_band_start[] = { 430000000, 860000000, 900000000 };
static const uint32_t s_band_end[]   = { 440000000, 870000000, 928000000 };

/* -------------------- spectrum analyzer -------------------- */

esp_err_t nesso_subghz_sweep(subghz_band_t band, subghz_spectrum_t *out)
{
    if (!out || band > SUBGHZ_BAND_915) return ESP_ERR_INVALID_ARG;

    const void *ctx = nesso_sx1262_get_hal_ctx();
    if (!ctx) return ESP_ERR_INVALID_STATE;

    out->band = band;
    out->freq_start_hz = s_band_start[band];
    out->freq_end_hz   = s_band_end[band];
    out->rssi_peak     = -128;
    out->peak_freq_hz  = out->freq_start_hz;

    uint32_t step = (out->freq_end_hz - out->freq_start_hz) / SUBGHZ_SPECTRUM_POINTS;

    /* Put radio in RX continuous for RSSI reads. */
    sx126x_set_standby(ctx, SX126X_STANDBY_CFG_RC);
    sx126x_set_pkt_type(ctx, SX126X_PKT_TYPE_LORA);
    sx126x_set_rx(ctx, 0);  /* continuous RX */

    for (int i = 0; i < SUBGHZ_SPECTRUM_POINTS; ++i) {
        uint32_t freq = out->freq_start_hz + (uint32_t)i * step;
        sx126x_set_rf_freq(ctx, freq);

        /* Brief settle time for PLL. */
        esp_rom_delay_us(200);

        /* Read instantaneous RSSI. */
        int8_t rssi = -128;
        sx126x_get_rssi_inst(ctx, &rssi);

        out->rssi[i] = rssi;
        if (rssi > out->rssi_peak) {
            out->rssi_peak = rssi;
            out->peak_freq_hz = freq;
        }
    }

    /* Back to standby. */
    sx126x_set_standby(ctx, SX126X_STANDBY_CFG_RC);

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

        int8_t rssi = -128;
        sx126x_get_rssi_inst(ctx, &rssi);

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
