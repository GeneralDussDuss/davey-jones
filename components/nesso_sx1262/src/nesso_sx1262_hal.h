/*
 * Private header — not exposed through include/.
 *
 * Defines the context struct the Semtech HAL callbacks get handed via
 * `const void *context`. Only the HAL implementation and the wrapper
 * (nesso_sx1262.c) touch this.
 */

#pragma once

#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_device_handle_t spi;         /* attached via nesso_spi_add_lora() */
    int                 busy_gpio;   /* NESSO_GPIO_LORA_BUSY */
    int                 dio1_gpio;   /* NESSO_GPIO_LORA_DIO1 */
} nesso_sx1262_hal_ctx_t;

#ifdef __cplusplus
}
#endif
