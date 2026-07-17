# Hardware boundary

本文件把**当前源码事实**与**仍待实物确认的事实**分开。它不是 PCB 原理图，也不证明当前硬件已复测。

## Source-confirmed GPIO map

| Subsystem | Module signal | GPIO | Code boundary |
|:--|:--|--:|:--|
| Microphone I²S0 | BCLK | 5 | `BOARD_I2S_MIC_BCLK` |
| Microphone I²S0 | WS/LRCLK | 4 | `BOARD_I2S_MIC_WS` |
| Microphone I²S0 | DIN from INMP441 | 6 | `BOARD_I2S_MIC_DIN` |
| Speaker I²S1 | BCLK | 15 | `BOARD_I2S_SPK_BCLK` |
| Speaker I²S1 | WS/LRCLK | 16 | `BOARD_I2S_SPK_WS` |
| Speaker I²S1 | DOUT to MAX98357A | 7 | `BOARD_I2S_SPK_DOUT` |
| ST7789 SPI2 | MOSI | 11 | `BOARD_LCD_PIN_MOSI` |
| ST7789 SPI2 | SCLK | 12 | `BOARD_LCD_PIN_SCLK` |
| ST7789 SPI2 | CS | 10 | `BOARD_LCD_PIN_CS` |
| ST7789 SPI2 | DC | 9 | `BOARD_LCD_PIN_DC` |
| ST7789 SPI2 | RST | 8 | `BOARD_LCD_PIN_RST` |
| ST7789 backlight | BL/enable | 13 | `BOARD_LCD_PIN_BL` |

The microphone code uses 16 kHz, 16-bit, mono and selects the left slot by default. INMP441 `L/R` wiring must match this slot; otherwise change the code after verifying the module.

## Power and electrical boundary

- Logic signals are treated as 3.3 V and all modules must share a common ground.
- The exact ESP32-S3 module, Flash/PSRAM size and development-board regulator are not confirmed from source alone.
- `sdkconfig.defaults` is designed around ESP32-S3, 16 MB Flash and Octal PSRAM. Do not flash it unchanged to an incompatible board.
- INMP441 should use a clean 3.3 V supply; microphone noise and speaker feedback materially affect the pipeline.
- MAX98357A VIN range, speaker impedance/power and peak current depend on the exact breakout and speaker. Do not power an unknown load from a GPIO.
- ST7789 VCC and backlight circuits vary between boards. GPIO13 may only be suitable for a logic-level BL/enable input, not direct high-current LED drive.
- USB power may be insufficient or noisy when Wi-Fi, LCD backlight and speaker are active together. Confirm regulator headroom and common ground on the retained prototype.

## Before flashing

1. Read the exact development-board and module labels.
2. Confirm Flash and PSRAM type/capacity.
3. Trace every GPIO against `board_pins.h`.
4. Confirm INMP441 L/R and the two I²S buses are not crossed.
5. Confirm MAX98357A VIN, speaker rating and power supply.
6. Confirm ST7789 resolution, controller, voltage and BL circuit.
7. Check for boot-strapping or board-specific reserved pins.
8. Use current-limited power for first bring-up.

## Current evidence

Source and clean ESP-IDF build are verified. The current publication pass did not flash a board, inspect power rails, capture audio, light the display, play speech or call live cloud services.
