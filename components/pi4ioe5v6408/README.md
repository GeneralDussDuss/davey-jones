# pi4ioe5v6408

ESP-IDF 5.3+ driver for the **Diodes PI4IOE5V6408** — an 8-bit I²C I/O expander.

Written for [DAVEY JONES](../../), the Arduino Nesso N1 pentesting firmware. The Nesso ships with **two** of these chips internally, so the driver is designed around multiple independent handles on the same I²C bus.

## Why this exists

No existing Ghost ESP / Bruce / Marauder board uses GPIO-over-I²C, but the Nesso N1 routes most of its interesting peripherals through two PI4IOE5V6408 expanders:

| Expander | Addr | Pins of interest on the Nesso |
|---|---|---|
| **E0** | `0x43` | KEY1 (P0), KEY2 (P1), LoRa LNA enable (P5), LoRa ant switch (P6), LoRa reset (P7) |
| **E1** | `0x44` | POWEROFF (P0), LCD reset (P1), GROVE power enable (P2), VIN detect (P5), LCD backlight (P6), built-in green LED (P7) |

So before we can bring up the LCD, handle a button press, power-cycle the LoRa radio, or turn the board off from software, we need this driver.

## Usage (Nesso N1 example)

```c
#include "driver/i2c_master.h"
#include "pi4ioe5v6408.h"

static i2c_master_bus_handle_t  bus;
static pi4ioe_handle_t          expander_e0;  // buttons + LoRa
static pi4ioe_handle_t          expander_e1;  // power + LCD + LED

void nesso_board_init(void)
{
    /* 1. Main internal I²C bus (SDA=GPIO10, SCL=GPIO8 on the Nesso). */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = 10,
        .scl_io_num = 8,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    /* 2. Attach both expanders. */
    pi4ioe_config_t e0_cfg = { .i2c_address = PI4IOE_ADDR_GND };
    pi4ioe_config_t e1_cfg = { .i2c_address = PI4IOE_ADDR_VDD };
    ESP_ERROR_CHECK(pi4ioe_create(bus, &e0_cfg, &expander_e0));
    ESP_ERROR_CHECK(pi4ioe_create(bus, &e1_cfg, &expander_e1));

    /* 3. Confirm both are alive. */
    bool e0_reset = false, e1_reset = false;
    ESP_ERROR_CHECK(pi4ioe_probe(expander_e0, &e0_reset));
    ESP_ERROR_CHECK(pi4ioe_probe(expander_e1, &e1_reset));

    /* 4. Buttons on E0: KEY1=P0, KEY2=P1. Inputs with pull-up. */
    ESP_ERROR_CHECK(pi4ioe_configure_input(expander_e0, 0, PI4IOE_PULL_UP));
    ESP_ERROR_CHECK(pi4ioe_configure_input(expander_e0, 1, PI4IOE_PULL_UP));

    /* 5. LoRa control on E0: LNA enable (P5), ant switch (P6), reset (P7).
     *    Start with LoRa reset asserted (low) and PA disabled. */
    ESP_ERROR_CHECK(pi4ioe_configure_output(expander_e0, 5, PI4IOE_LEVEL_LOW));  // LNA off
    ESP_ERROR_CHECK(pi4ioe_configure_output(expander_e0, 6, PI4IOE_LEVEL_LOW));  // ant → RX
    ESP_ERROR_CHECK(pi4ioe_configure_output(expander_e0, 7, PI4IOE_LEVEL_LOW));  // LoRa in reset

    /* 6. Power / UI on E1: LCD reset (P1), LCD backlight (P6), LED (P7). */
    ESP_ERROR_CHECK(pi4ioe_configure_output(expander_e1, 1, PI4IOE_LEVEL_LOW));  // LCD in reset
    ESP_ERROR_CHECK(pi4ioe_configure_output(expander_e1, 6, PI4IOE_LEVEL_LOW));  // backlight off
    ESP_ERROR_CHECK(pi4ioe_configure_output(expander_e1, 7, PI4IOE_LEVEL_LOW));  // LED off

    /* 7. POWEROFF (E1.P0) stays input-with-pull-down for now; we'll pulse
     *    it later from a menu entry. Never let it float. */
    ESP_ERROR_CHECK(pi4ioe_configure_input(expander_e1, 0, PI4IOE_PULL_DOWN));
}

/* Release LCD from reset, backlight on. */
void nesso_lcd_power_up(void)
{
    pi4ioe_set_level(expander_e1, 1, PI4IOE_LEVEL_HIGH);   // LCD out of reset
    pi4ioe_set_level(expander_e1, 6, PI4IOE_LEVEL_HIGH);   // backlight on
}

/* Read KEY1 (active-low with pull-up). */
bool nesso_key1_pressed(void)
{
    pi4ioe_level_t lvl;
    if (pi4ioe_get_level(expander_e0, 0, &lvl) != ESP_OK) return false;
    return lvl == PI4IOE_LEVEL_LOW;
}

/* Toggle the LoRa antenna switch for TX. */
void nesso_lora_switch_to_tx(void)
{
    pi4ioe_set_level(expander_e0, 6, PI4IOE_LEVEL_HIGH);
    pi4ioe_set_level(expander_e0, 5, PI4IOE_LEVEL_HIGH);   // enable LNA/PA path
}
```

## Key chip quirks the driver handles

1. **Output config ordering.** The chip wants you to touch registers in this order when enabling an output: `OUTPUT_HI_Z` → `OUTPUT_STATE` → `IO_DIRECTION`. `pi4ioe_configure_output()` does it for you.
2. **Reading `INTERRUPT_STATUS` clears every latched flag.** The chip can't clear bits individually. Budget your interrupt handling around this — you can't "peek" without clearing.
3. **`INTERRUPT_MASK` is inverted** from how you'd expect. Chip uses 1 = masked. The driver's `pi4ioe_set_interrupt_mask()` takes a user-friendly enable mask and flips it for you.
4. **Device ID validation.** `pi4ioe_probe()` checks the top 3 bits of register 0x01 for the expected `0b101` signature. Also returns whether the chip saw a power-on reset flag.
5. **Not 5 V tolerant.** Driver doesn't care, but if you wire anything to the expander pins externally, keep it at 3.3 V. The chip is happy up to ~4.1 V on inputs; beyond that you'll cook it.

## Thread safety

Each handle carries its own mutex. The ESP-IDF i2c_master layer serializes bus access, so running E0 and E1 operations from different tasks is safe.

## License

BSD-3-Clause. Register map and ordering constraints verified against the BSD-3-licensed MicroPython driver at [codeberg.org/easytarget/pi4ioe5v6408-micropython](https://codeberg.org/easytarget/pi4ioe5v6408-micropython). Original C implementation.
