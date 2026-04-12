/*
 * nesso_spi.c — shared SPI bus owner for the Nesso N1's LCD + LoRa radio.
 */

#include "nesso_spi.h"
#include "nesso_bsp.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "nesso_spi";

static bool s_bus_ready = false;

/* Max DMA transfer size — LCD may send a full frame buffer at once.
 * 135 × 240 × 2 bytes = 64,800, round up to a power-of-two-ish value. */
#define NESSO_SPI_MAX_TRANSFER_SZ  (72 * 1024)

esp_err_t nesso_spi_init(void)
{
    if (s_bus_ready) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    const spi_bus_config_t bus_cfg = {
        .mosi_io_num     = NESSO_GPIO_SPI_MOSI,
        .miso_io_num     = NESSO_GPIO_SPI_MISO,
        .sclk_io_num     = NESSO_GPIO_SPI_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = NESSO_SPI_MAX_TRANSFER_SZ,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(NESSO_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize failed");

    s_bus_ready = true;
    ESP_LOGI(TAG, "shared bus up on SPI%d (MOSI=%d SCK=%d MISO=%d)",
             NESSO_SPI_HOST, NESSO_GPIO_SPI_MOSI, NESSO_GPIO_SPI_SCK,
             NESSO_GPIO_SPI_MISO);
    return ESP_OK;
}

esp_err_t nesso_spi_deinit(void)
{
    if (!s_bus_ready) return ESP_OK;
    esp_err_t err = spi_bus_free(NESSO_SPI_HOST);
    if (err == ESP_OK) s_bus_ready = false;
    return err;
}

esp_err_t nesso_spi_add_lcd(int clock_hz, spi_device_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(s_bus_ready, ESP_ERR_INVALID_STATE, TAG,
                        "call nesso_spi_init() first");
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG, "null handle");

    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = clock_hz > 0 ? clock_hz : 40 * 1000 * 1000, /* 40 MHz default */
        .mode           = 0,
        .spics_io_num   = NESSO_GPIO_LCD_CS,
        .queue_size     = 8,
        /* LCD is write-only. Full-duplex is fine; LVGL pushes large transfers. */
    };
    return spi_bus_add_device(NESSO_SPI_HOST, &dev_cfg, out_handle);
}

esp_err_t nesso_spi_add_lora(int clock_hz, spi_device_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(s_bus_ready, ESP_ERR_INVALID_STATE, TAG,
                        "call nesso_spi_init() first");
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG, "null handle");

    /* SX1262 max SPI clock is 16 MHz. Default to 8 MHz for safety with
     * non-ideal routing through shared traces. */
    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = clock_hz > 0 ? clock_hz : 8 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = NESSO_GPIO_LORA_CS,
        .queue_size     = 4,
    };
    return spi_bus_add_device(NESSO_SPI_HOST, &dev_cfg, out_handle);
}

esp_err_t nesso_spi_remove(spi_device_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return spi_bus_remove_device(handle);
}
