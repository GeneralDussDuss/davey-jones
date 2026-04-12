/*
 * nesso_lcd — ST7789 LCD bring-up for the Arduino Nesso N1.
 *
 * The Nesso has a 1.14" 135×240 IPS panel driven by an ST7789P3. Reset and
 * backlight are on expander E1, not direct GPIO, so this component hooks
 * ESP-IDF's esp_lcd_panel_st7789 driver but routes power/reset control
 * through nesso_bsp instead of letting esp_lcd touch GPIO directly.
 *
 * Shared SPI bus (MOSI=21, SCK=20) is owned by nesso_spi; we just reuse
 * the host number here.
 *
 * Typical use:
 *     nesso_bsp_init();
 *     nesso_spi_init();
 *     nesso_lcd_init();
 *     esp_lcd_panel_handle_t panel = nesso_lcd_panel();
 *     // …push pixels via esp_lcd_panel_draw_bitmap() or hand to LVGL.
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native panel dimensions. 135×240, portrait-up default. */
#define NESSO_LCD_WIDTH             135
#define NESSO_LCD_HEIGHT            240
#define NESSO_LCD_BITS_PER_PIXEL    16

/*
 * ST7789 frame-buffer offsets for the Nesso's 135×240 die.
 * These match the M5StickC Plus and LilyGO T-Display — same die family,
 * same offsets. Portrait orientation, display origin at (52, 40).
 */
#define NESSO_LCD_X_GAP             52
#define NESSO_LCD_Y_GAP             40

/*
 * SPI clock. 40 MHz is what the Arduino driver uses and what the panel
 * comfortably sustains. Drop to 20 MHz if we ever see corruption.
 */
#define NESSO_LCD_SPI_CLOCK_HZ      (40 * 1000 * 1000)

/**
 * Bring up the LCD:
 *   1. Allocate a panel-IO SPI device on the shared host.
 *   2. Pulse reset via the expander.
 *   3. Run the ST7789 init sequence.
 *   4. Apply panel offsets, invert color, turn display on.
 *   5. Turn the backlight on via the expander.
 *
 * Requires nesso_bsp_init() and nesso_spi_init() to have already run.
 */
esp_err_t nesso_lcd_init(void);

/** Shut the LCD down — display off, backlight off, held in reset. */
esp_err_t nesso_lcd_deinit(void);

/**
 * Panel handle for draw calls / LVGL flush callbacks.
 * Returns NULL if nesso_lcd_init() hasn't been called or failed.
 */
esp_lcd_panel_handle_t nesso_lcd_panel(void);

/** Underlying panel-IO handle, exposed for registering transaction callbacks. */
esp_lcd_panel_io_handle_t nesso_lcd_panel_io(void);

#ifdef __cplusplus
}
#endif
