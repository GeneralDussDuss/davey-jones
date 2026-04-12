/*
 * nesso_buzzer.c — LEDC PWM on GPIO11 for the Nesso's passive buzzer.
 */
#include "nesso_buzzer.h"
#include "nesso_bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "buzzer";
static bool s_initted = false;

#define BUZZER_TIMER   LEDC_TIMER_0
#define BUZZER_CHANNEL LEDC_CHANNEL_0
#define BUZZER_SPEED   LEDC_LOW_SPEED_MODE

esp_err_t nesso_buzzer_init(void)
{
    if (s_initted) return ESP_OK;

    ledc_timer_config_t timer = {
        .speed_mode      = BUZZER_SPEED,
        .timer_num       = BUZZER_TIMER,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz         = 2000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "timer");

    ledc_channel_config_t ch = {
        .speed_mode = BUZZER_SPEED,
        .channel    = BUZZER_CHANNEL,
        .timer_sel  = BUZZER_TIMER,
        .gpio_num   = NESSO_GPIO_BUZZER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "channel");

    s_initted = true;
    return ESP_OK;
}

esp_err_t nesso_buzzer_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!s_initted) nesso_buzzer_init();
    if (freq_hz < 100) freq_hz = 100;
    if (freq_hz > 20000) freq_hz = 20000;

    ledc_set_freq(BUZZER_SPEED, BUZZER_TIMER, freq_hz);
    ledc_set_duty(BUZZER_SPEED, BUZZER_CHANNEL, 128); /* 50% duty */
    ledc_update_duty(BUZZER_SPEED, BUZZER_CHANNEL);

    if (duration_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        ledc_set_duty(BUZZER_SPEED, BUZZER_CHANNEL, 0);
        ledc_update_duty(BUZZER_SPEED, BUZZER_CHANNEL);
    }
    return ESP_OK;
}

esp_err_t nesso_buzzer_off(void)
{
    if (!s_initted) return ESP_OK;
    ledc_set_duty(BUZZER_SPEED, BUZZER_CHANNEL, 0);
    ledc_update_duty(BUZZER_SPEED, BUZZER_CHANNEL);
    return ESP_OK;
}
