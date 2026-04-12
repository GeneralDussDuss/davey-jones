/*
 * nesso_ir — IR transmitter for DAVEY JONES.
 *
 * Uses the ESP32-C6 RMT peripheral on GPIO9 (built-in IR LED on the Nesso).
 * Supports NEC protocol encoding and a TV-B-Gone mode that cycles through
 * common power-off codes.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the IR TX channel on GPIO9. Call once. */
esp_err_t nesso_ir_init(void);

/** Deinit and release the RMT channel. */
esp_err_t nesso_ir_deinit(void);

/** Send a raw NEC code (address + command). */
esp_err_t nesso_ir_send_nec(uint16_t address, uint8_t command);

/** Send a raw Samsung code. */
esp_err_t nesso_ir_send_samsung(uint16_t address, uint8_t command);

/**
 * Start TV-B-Gone mode: cycles through ~20 common power-off codes
 * with 200ms between each. Blocks until all codes sent.
 * Returns number of codes transmitted.
 */
int nesso_ir_tvbgone(void);

/** True if IR is initialized. */
bool nesso_ir_is_ready(void);

#ifdef __cplusplus
}
#endif
