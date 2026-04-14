# DAVEY JONES

Multi-radio pentesting firmware for the **Arduino Nesso N1** (ESP32-C6 + SX1262 LoRa). A Flipper Zero alternative built from scratch on ESP-IDF.

> Named after the keeper of the deep. What's yours is now his.

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-C6 (RISC-V, WiFi 6, BLE 5.0, 802.15.4) |
| Sub-GHz | Semtech SX1262 LoRa (850-960 MHz) |
| Display | ST7789 135x240 IPS LCD |
| Touch | FT6336 capacitive touchscreen |
| I/O | 2x PI4IOE5V6408 I2C GPIO expanders |
| Storage | 16MB flash (4MB app + ~12MB SPIFFS) |
| Other | Passive buzzer, RGB LED, USB-C |

## Features

### WiFi (802.11ax)
- **Wardrive** - Channel-hopping beacon capture, Wigle CSV export
- **PMKID Capture** - EAPOL key sniffing, hashcat 22000 format output
- **Deauth** - Targeted deauthentication attacks with frame counter
- **Beacon Spam** - Broadcast fake AP networks
- **Evil Portal** - Captive portal phishing (Google, Facebook, Microsoft, Free WiFi templates)
- **Daveygotchi** - Autonomous PMKID hunter with personality and mood states

### Bluetooth Low Energy
- **BLE Scanner** - Passive scan with device type detection (Apple, Samsung, Google, HID, etc.)
- **Bad-KB** - Full GATT HID keyboard with 11 device disguises (spoofed names + OUI MACs) and 18 payloads (rickroll, reverse shell, matrix rain, etc.)
- **BLE Spam** - Apple/Samsung/Google/Windows popup floods (20+ device models)
- **Tracker Detector** - AirTag, SmartTag, Tile detection with buzzer alert
- **BLE Flood** - Rapid connection attempts from random MACs
- **Device Cloner** - Clone and rebroadcast BLE advertisements
- **Sniffer** - Log all BLE advertisements to CSV
- **iBeacon** - Broadcast custom iBeacon frames

### Sub-GHz (850-960 MHz)
- **Spectrum Analyzer** - Live RSSI waveform with audio feedback (Flipper-style chirp)
- **Signal Capture** - OOK bitstream recording at target frequency
- **Signal Replay** - Retransmit captured signals
- **LoRa Sniffer** - Passive LoRa packet capture at 915 MHz
- **LoRa Chat** - Text messaging over LoRa

### Infrared
- **TV-B-Gone** - 20+ power codes, continuous loop
- **Samsung Remote** - Virtual remote control
- **Volume MAX** - Crank volume to max on nearby TVs
- **Channel Chaos** - Rapid channel switching

### Zigbee / Thread (802.15.4)
- **Channel Scanner** - Hop channels 11-26, extract device info
- **Packet Logger** - Log raw 802.15.4 frames

### Easter Eggs
- **The Locker** - Hidden GIF animation player
- **The Salty Deep** - BLE toy scanner and controller (Lovense protocol)

## Architecture

```
main/main.c              Boot: hardware init + UI only (radios are lazy)
components/
  nesso_bsp/             Board support: I2C expanders, pin routing
  nesso_spi/             Shared SPI bus (LCD + LoRa)
  nesso_lcd/             ST7789 display driver
  nesso_buttons/         Debounced button polling (20Hz, FreeRTOS task)
  nesso_ui/              LVGL 9.x menu system (~2800 lines, 30+ screens)
  nesso_wifi/            WiFi STA + promiscuous subscriber fanout
  nesso_wardrive/        Channel-hopping beacon logger
  nesso_eapol/           EAPOL/PMKID capture + hashcat export
  nesso_portal/          Evil captive portal + DNS hijack
  nesso_ble/             Full BLE toolkit (NimBLE stack)
  nesso_subghz/          Sub-GHz sweep/capture/replay/LoRa
  nesso_sx1262/          SX1262 HAL + driver (vendored Semtech)
  nesso_ir/              IR blaster (RMT peripheral, NEC + Samsung)
  nesso_buzzer/          LEDC PWM buzzer
  nesso_zigbee/          802.15.4 sniffer (esp_ieee802154)
  pi4ioe5v6408/          I2C GPIO expander driver (BSD-3-Clause)
tools/
  gif_to_frames.py       Convert GIFs to RGB565 C arrays
```

### Lazy Radio Management

Radios activate only when you enter their menu. Switching domains (WiFi -> BLE -> Sub-GHz) tears down the old radio and frees RAM before starting the new one. The ESP32-C6 has ~300KB SRAM — this architecture keeps ~150KB free at idle so each radio has room to breathe.

| Domain | Activates on | Tears down |
|--------|-------------|------------|
| WiFi | WiFi menu entry | Wardrive + EAPOL tasks, promisc subscribers |
| BLE | Bluetooth menu entry | NimBLE stack, GATT services |
| Sub-GHz | Sub-GHz menu entry | SX1262 driver |
| Zigbee | Zigbee menu entry | 802.15.4 radio |

## Controls

| Input | Action |
|-------|--------|
| KEY1 (press) | Scroll / cycle through items |
| KEY1 (long press) | Emergency stop + return to main menu |
| KEY2 (press) | Back |
| Touchscreen tap | Select (on menu screens) |
| Touchscreen double-tap | Select (on non-menu screens, safety) |
| Touchscreen swipe right | Back |

## Building

### Prerequisites
- [Espressif ESP-IDF v5.3.2](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32c6/get-started/index.html) (Windows installer recommended)
- Arduino Nesso N1 connected via USB-C

### Build & Flash

**PowerShell (recommended):**
```powershell
.\build.ps1           # Build
# Then flash:
idf.py -p COM15 flash
```

**Or manually:**
```bash
idf.py set-target esp32c6
idf.py build
idf.py -p COM15 flash
idf.py -p COM15 monitor   # Serial console
```

### First Flash
If you hit boot loops after changing sdkconfig, erase flash first:
```bash
esptool.py --chip esp32c6 erase_flash
idf.py -p COM15 flash
```

## Dev Notes

### Hardware Gotchas
- **SX1262 RF path**: DIO2 controls the antenna switch automatically (`sx126x_set_dio2_as_rf_sw_ctrl(true)`). The LNA enable (E0.P5) and RF switch enable (E0.P6) are set HIGH once at BSP init and left alone.
- **No 433 MHz**: The Nesso N1's SX1262 module only covers 850-960 MHz. Don't waste time trying 433.
- **No TCXO**: Module uses 32 MHz crystal, not TCXO. Set `tcxo_enable = false`.
- **Touch GPIO3 conflict**: FT6336 INT and BMI270 INT share GPIO3. Use polling mode (`int_gpio = -1`) for touch.
- **SX1262 frequency changes**: Can't change frequency while in RX mode. Must go standby -> set_freq -> RX for each step.
- **RSSI type**: `sx126x_get_rssi_inst()` writes `int16_t`, not `int8_t`. Pass the right type or get memory corruption.

### Memory Management
- ESP32-C6 has ~300KB SRAM. WiFi alone eats ~100KB. BLE needs ~50KB to init.
- WiFi promisc + wardrive + EAPOL running simultaneously leaves only ~15KB free — not enough for BLE.
- Solution: lazy radio domains. Only one radio stack active at a time.
- LVGL uses a single shared canvas buffer (240*135*2 = ~64KB). Previous dual-buffer approach wasted 39KB.

### Known Limitations
- Bad-KB HID pairing can be flaky on some Windows versions — may need to "forget" and re-pair
- TV-B-Gone and BLE toy scan block the LVGL timer during operation (one-shot screens, accepted tradeoff)
- Zigbee scanning depends on nearby devices actually transmitting on the scanned channel
- `esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)` can corrupt NimBLE init on ESP32-C6 — we skip it entirely since C6 has no Classic BT

### Troubleshooting

**"WiFi menu freezes the device":** Fixed in latest — radio transitions now run on a background task. Pull and reflash.

**"Build script can't find ESP-IDF":** The script auto-detects from common paths. Override with `$env:IDF_PATH = "path\to\esp-idf"` before running.

**"Bad-KB won't pair":** Make sure you're entering the Bad-KB menu *before* trying to pair (lazy init needs BLE domain active). On Windows, remove the device under Settings > Bluetooth if you tried pairing while GATT services weren't registered yet.

**"BLE scan shows no devices":** The device was in WiFi domain. Navigate into Bluetooth menu first — it switches domains and frees the RAM BLE needs (~60KB).

### Promiscuous WiFi Multi-Subscriber
The WiFi layer supports up to 4 simultaneous promiscuous callbacks via a fanout system. Wardrive and EAPOL capture register as separate subscribers with independent filters, both receiving packets from the same radio. Filter mask `0` maps to `WIFI_PROMIS_FILTER_MASK_ALL` (0xFFFFFFFF), not "capture nothing."

### Build System
The `build.ps1` script exists because ESP-IDF's toolchain conflicts with MSYS/MinGW environment variables inherited from Git Bash on Windows. It clears those vars, sets up the IDF Python venv, and runs `idf.py`.

## Legal

This tool is intended for **authorized security testing, research, and educational purposes only**. Users are responsible for complying with all applicable laws. Do not use against networks or devices you don't own or have explicit authorization to test.

## License

MIT
