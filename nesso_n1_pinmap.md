# Arduino Nesso N1 — Full Pinmap

**SKU**: TPX00227
**Source**: Arduino official full-pinout PDF (last update 2025-11-21)
**MCU**: ESP32-C6 (RISC-V, WiFi 6 / BLE 5 / Zigbee / Thread / Matter)
**Logic level**: 3.3 V only (NOT 5 V tolerant)

## I/O Expanders — CRITICAL

Two PI4IOE5V6408 I²C expanders sit on the main I²C bus. **Many peripherals are behind them, not on direct ESP32-C6 GPIO.** This is the biggest porting gotcha.

| Expander | I²C Addr | Role |
|---|---|---|
| **E0** | `0x43` | Buttons (KEY1, KEY2) + LoRa control (reset, antenna switch, LNA enable) |
| **E1** | `0x44` | Power control / UI: LCD reset, LCD backlight, GROVE power, VIN detect, built-in LED, POWEROFF |

## Main I²C Bus (shared by touch, IMU, both expanders)

| Signal | ESP32-C6 |
|---|---|
| SDA | **GPIO10** |
| SCL | **GPIO8** |

I²C devices on this bus:
| Device | Addr | Purpose |
|---|---|---|
| FT6336 | `0x38` | Capacitive touch controller |
| BMI270 | `0x68` | 6-axis IMU |
| PI4IOE5V6408 | `0x43` | Expander E0 |
| PI4IOE5V6408 | `0x44` | Expander E1 |

## Shared SPI Bus (LCD + LoRa on same host)

| Signal | ESP32-C6 |
|---|---|
| MOSI | **GPIO21** |
| SCK  | **GPIO20** |
| MISO | **GPIO22** (LoRa only — LCD is write-only) |

**Both LCD and LoRa share MOSI/SCK.** Must use ESP-IDF SPI bus with two device handles on different CS pins.

## LCD — ST7789, 135×240 IPS

| Signal | Routing |
|---|---|
| CS | **GPIO17** |
| DC / RS | **GPIO16** |
| MOSI | GPIO21 (shared SPI) |
| SCK | GPIO20 (shared SPI) |
| RESET | **E1.P1** (via expander) ⚠ |
| BACKLIGHT | **E1.P6** (via expander) ⚠ |

## Touch — FT6336, I²C addr 0x38

| Signal | ESP32-C6 |
|---|---|
| SDA | GPIO10 |
| SCL | GPIO8 |
| INT | **GPIO3** |

## IMU — BMI270, I²C addr 0x68

| Signal | ESP32-C6 |
|---|---|
| SDA | GPIO10 |
| SCL | GPIO8 |
| INT | GPIO3 *(pinout PDF lists same as touch — likely shared IRQ line or doc quirk; verify with scope)* |

## LoRa — Semtech SX1262 (850–960 MHz)

| Signal | Routing |
|---|---|
| MOSI | GPIO21 (shared SPI) |
| SCK | GPIO20 (shared SPI) |
| MISO | **GPIO22** |
| CS | **GPIO23** |
| BUSY | **GPIO19** |
| DIO1 (IRQ) | **GPIO15** |
| RESET | **E0.P7** (expander) ⚠ |
| ANTENNA SWITCH | **E0.P6** (expander) ⚠ |
| LNA ENABLE | **E0.P5** (expander) ⚠ |

Antenna: MMCX connector. **Never transmit without antenna connected — damages PA.**

## Buttons — ALL on Expander E0/E1 ⚠

| Button | Routing |
|---|---|
| KEY1 (top user button) | **E0.P0** |
| KEY2 (back user button) | **E0.P1** |
| POWER / POWEROFF | **E1.P0** |

**Power button semantics (from datasheet):**
- Click (off state) → power on
- Click (on state) → reset
- **Double-click** → shut down
- Long press → enter bootloader

## LED / Buzzer / IR

| Peripheral | Routing |
|---|---|
| Built-in green LED | **E1.P7** (expander) ⚠ |
| Buzzer (passive) | **GPIO11** (direct, PWM-capable) |
| IR TX emitter | **GPIO9** (direct) |

## USB-C

| Signal | ESP32-C6 |
|---|---|
| USB D- | GPIO12 |
| USB D+ | GPIO13 |

Used for: programming (native USB JTAG/CDC) + LiPo charging.

## Grove Connector (4-pin)

| Pin | Signal | ESP32-C6 |
|---|---|---|
| 1 | GND | — |
| 2 | +5V (switched) | — |
| 3 | GROVE_IO_0 / SDA | **GPIO5** |
| 4 | GROVE_IO_1 / SCL | **GPIO4** |

Power enable: **E1.P2** (expander) — must be turned on before Grove module works.

Note: GPIO5/4 are *separate* from the main I²C bus (which is GPIO10/8). So Grove has its own I²C that doesn't conflict with internal touch/IMU/expanders. Useful.

## Qwiic Connector (4-pin)

Same as main internal I²C bus:
| Pin | Signal | ESP32-C6 |
|---|---|---|
| 1 | GND | — |
| 2 | +3V3 | — |
| 3 | SDA | **GPIO10** (shared with internal) |
| 4 | SCL | **GPIO8** (shared with internal) |

⚠ Any Qwiic module goes on the **same bus** as touch / IMU / expanders. Watch for address collisions.

## 8-Pin Expansion Header (M5StickC HAT compatible)

| Pin | Function | ESP32-C6 |
|---|---|---|
| 1 | GND | — |
| 2 | +5V (out) | — |
| 3 | ~D1 | **GPIO7** |
| 4 | ~D2 | **GPIO2** |
| 5 | ~D3 | **GPIO6** |
| 6 | BATTERY (out, ~3–4.2 V, unregulated, output-only — don't back-feed) | — |
| 7 | +3V3 (out) | — |
| 8 | +5V (in) | — |

## Battery

- 250 mAh LiPo, built-in
- USB/VIN detect on **E1.P5** (expander)

## Free / Unaccounted GPIOs (ESP32-C6 has 31 total, 0–30)

Used directly: 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 19, 20, 21, 22, 23

Free / debug / unknown: 0, 1, 14, 18, 24–30

## Porting Implications for Ghost ESP

1. **I/O expander driver is a hard requirement.** Ghost's stock drivers assume direct GPIO for LCD reset/backlight, buttons, LED. None of those work here. We need a PI4IOE5V6408 component + a shim that Ghost's LVGL/button/LED code can call.

2. **ST7789 init sequence** must toggle reset via E1.P1 and control backlight via E1.P6 instead of direct GPIO. Requires either patching LVGL's ST7789 driver or wrapping it.

3. **Shared SPI bus** between LCD and SX1262 — use ESP-IDF SPI with two device handles. Standard but must be wired correctly.

4. **SX1262 TX/RX needs expander coordination** — E0.P5 (LNA), E0.P6 (ant switch), E0.P7 (reset) have to be driven around every transmit/receive state transition.

5. **Power-off path**: the Nesso shuts down by the PMIC (E1.P0 POWEROFF). A "shutdown" menu entry in Ghost would need to drive that expander pin.

6. **Grove = separate I²C** on GPIO5/4, free of the internal bus collisions. Good place to add a Qwiic GPS module for wardriving without load on the main bus.
