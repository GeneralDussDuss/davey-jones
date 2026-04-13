/*
 * nesso_subghz — Sub-GHz spectrum analysis, capture, and replay.
 *
 * Uses the SX1262 LoRa radio in FSK/OOK mode for sub-GHz work:
 *   - Spectrum analyzer: sweep frequencies, display RSSI as live waveform
 *   - Signal capture: record OOK bit patterns at a target frequency
 *   - Signal replay: retransmit captured signals
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------- spectrum analyzer -------------------- */

typedef enum {
    SUBGHZ_BAND_WIDE = 0, /* 850-960 MHz — full Nesso SX1262 range */
    SUBGHZ_BAND_868,       /* 860-870 MHz — ISM EU, LoRaWAN EU */
    SUBGHZ_BAND_915,       /* 902-928 MHz — ISM Americas, LoRaWAN US */
    SUBGHZ_BAND_COUNT,
    /* NOTE: Nesso N1 SX1262 module is 850-960 MHz ONLY.
     * 433 MHz is NOT supported by this hardware. */
} subghz_band_t;

#define SUBGHZ_SPECTRUM_POINTS 240  /* landscape LCD width */

typedef struct {
    subghz_band_t band;
    uint32_t      freq_start_hz;
    uint32_t      freq_end_hz;
    int8_t        rssi[SUBGHZ_SPECTRUM_POINTS];  /* dBm per point */
    int8_t        rssi_peak;
    uint32_t      peak_freq_hz;
} subghz_spectrum_t;

/** Run one spectrum sweep. Blocks for ~50-100ms. */
esp_err_t nesso_subghz_sweep(subghz_band_t band, subghz_spectrum_t *out);

/* -------------------- capture -------------------- */

#define SUBGHZ_CAPTURE_MAX_BYTES 256

typedef struct {
    uint32_t freq_hz;
    uint8_t  data[SUBGHZ_CAPTURE_MAX_BYTES];
    size_t   length;        /* bytes captured */
    uint32_t duration_ms;   /* capture duration */
} subghz_capture_t;

/**
 * Capture OOK signal at the given frequency for up to duration_ms.
 * Uses RSSI threshold to detect signal presence and records timing.
 * Returns ESP_OK if signal detected, ESP_ERR_TIMEOUT if nothing heard.
 */
esp_err_t nesso_subghz_capture(uint32_t freq_hz, uint32_t duration_ms,
                                int8_t rssi_threshold,
                                subghz_capture_t *out);

/** Save a capture to SPIFFS. */
esp_err_t nesso_subghz_save(const subghz_capture_t *cap, const char *path);

/** Load a capture from SPIFFS. */
esp_err_t nesso_subghz_load(const char *path, subghz_capture_t *out);

/* -------------------- replay -------------------- */

/** Replay a previously captured signal. */
esp_err_t nesso_subghz_replay(const subghz_capture_t *cap);

/* -------------------- LoRa sniffer -------------------- */

#define LORA_SNIFF_MAX_PKTS 16

typedef struct {
    uint8_t  data[64];
    uint8_t  length;
    int8_t   rssi;
    int8_t   snr;
    uint32_t timestamp;
} lora_sniff_pkt_t;

typedef struct {
    lora_sniff_pkt_t packets[LORA_SNIFF_MAX_PKTS];
    size_t   count;
    uint32_t total_seen;
    bool     running;
} lora_sniff_state_t;

/** Start LoRa packet sniffer at given frequency with given SF/BW. */
esp_err_t nesso_lora_sniff_start(uint32_t freq_hz, uint8_t sf, uint8_t bw);
esp_err_t nesso_lora_sniff_stop(void);
esp_err_t nesso_lora_sniff_get(lora_sniff_state_t *out);

/* -------------------- LoRa TX (chat / beacon) -------------------- */

/** Send a LoRa text message. */
esp_err_t nesso_lora_send(uint32_t freq_hz, const char *msg);

#ifdef __cplusplus
}
#endif
