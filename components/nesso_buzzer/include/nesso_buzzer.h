/*
 * nesso_buzzer — passive buzzer on GPIO11 via LEDC PWM.
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nesso_buzzer_init(void);
esp_err_t nesso_buzzer_tone(uint32_t freq_hz, uint32_t duration_ms);
esp_err_t nesso_buzzer_off(void);

#ifdef __cplusplus
}
#endif
