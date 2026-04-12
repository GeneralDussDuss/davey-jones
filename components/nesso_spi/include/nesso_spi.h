/*
 * nesso_spi — shared SPI bus owner for DAVEY JONES.
 *
 * The Nesso N1 shares a single SPI bus between the ST7789 LCD and the
 * SX1262 LoRa radio:
 *
 *     MOSI  = GPIO21
 *     SCK   = GPIO20
 *     MISO  = GPIO22    (LoRa only — LCD is write-only)
 *
 *     LCD  CS = GPIO17, DC = GPIO16
 *     LoRa CS = GPIO23
 *
 * We therefore run ONE spi_bus_initialize() at boot and hand out two device
 * handles via spi_bus_add_device(). The LCD and LoRa drivers call the two
 * helpers in this header instead of opening their own SPI host.
 *
 * SPI host choice: ESP32-C6 exposes SPI2_HOST (GPSPI2) as the only user host.
 * SPI0/SPI1 are reserved for flash and cache.
 */

#pragma once

#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SPI host that drives both LCD and LoRa. */
#define NESSO_SPI_HOST  SPI2_HOST

/** Initialize the shared SPI bus. Call once, after nesso_bsp_init(). */
esp_err_t nesso_spi_init(void);

/** Tear down the shared SPI bus and drop any attached devices. */
esp_err_t nesso_spi_deinit(void);

/** Attach the ST7789 LCD. Returns a device handle the LCD driver uses. */
esp_err_t nesso_spi_add_lcd(int clock_hz, spi_device_handle_t *out_handle);

/** Attach the SX1262 LoRa radio. Returns a device handle the LoRa driver uses. */
esp_err_t nesso_spi_add_lora(int clock_hz, spi_device_handle_t *out_handle);

/** Detach a previously-added device. */
esp_err_t nesso_spi_remove(spi_device_handle_t handle);

#ifdef __cplusplus
}
#endif
