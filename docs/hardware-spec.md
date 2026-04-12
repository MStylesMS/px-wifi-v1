# PX-WiFi-V1 — Hardware Specification

**Version:** 0.1 Draft  
**Date:** 2026-04-07  
**Status:** DRAFT — Component selections are placeholders. Review and update with actual parts.  

Reference: https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html

---

## 1. Microcontroller

| Parameter | Value |
|-----------|-------|
| Module | ESP32-S3-WROOM-1 (or DevKitC-1 v1.0 for prototype) |
| CPU | Dual-core Xtensa LX7, 240 MHz |
| Flash | 8 MB (quad SPI) |
| PSRAM | 2 MB (optional, for future audio buffering) |
| WiFi | 802.11 b/g/n, 2.4 GHz |
| Bluetooth | BLE 5.0 (available but not used in v1.0) |
| Operating voltage | 3.3V |
| GPIO count | 45 (of which ~36 usable) |
| ADC | 2× 12-bit SAR ADC, up to 20 channels |
| DAC | None native on S3 — use PWM or I2S for audio |
| I2C | 2× hardware I2C controllers |
| SPI | 4× SPI controllers (2 usable for general purpose) |

**Note:** The ESP32-S3 does NOT have a true DAC. Audio output for the buzzer will use either PWM (LEDC) for tone generation or I2S with an external DAC for richer audio. For v1.0 (piezo buzzer), PWM is sufficient.

---

## 2. Power Supply

| Parameter | Value |
|-----------|-------|
| Input voltage | 6–12V DC |
| Connector | TBD — barrel jack (2.1mm) or screw terminal |
| 5V regulator | TBD — LDO or buck converter (e.g., AMS1117-5.0 or MP1584) |
| 5V rail current | ≥500 mA (for display + peripherals) |
| 3.3V regulator | Onboard ESP32 module regulator (or external LDO) |
| 3.3V rail current | ≥500 mA |
| Battery monitoring | Voltage divider on ADC input for battery level monitoring |
| Low battery threshold | 6.5V (configurable) |

### Power Budget Estimate

| Component | Typical Current (5V) | Typical Current (3.3V) |
|-----------|----------------------|------------------------|
| ESP32-S3 (WiFi active) | — | ~240 mA |
| 7-segment display (I2C) | ~20 mA | — |
| Piezo buzzer | ~30 mA | — |
| WS2812 status LED | ~20 mA | — |
| GPIO pull-ups (8×) | — | ~1 mA |
| Low-power outputs (4× LEDs) | ~80 mA | — |
| **Total estimate** | **~150 mA** | **~241 mA** |

---

## 3. Pin Assignment

### 3.1 GPIO Inputs (Wiring Harness — 8 channels)

| Index | Pin Name | Name (default) | Notes |
|-------|----------|----------------|-------|
| 1 | `INPUT_1` | `red` | Internal pull-up, active low. Switchable to output. |
| 2 | `INPUT_2` | `green` | Internal pull-up, active low. Switchable to output. |
| 3 | `INPUT_3` | `yellow` | Internal pull-up, active low. Switchable to output. |
| 4 | `INPUT_4` | `blue` | Internal pull-up, active low. Switchable to output. |
| 5 | `INPUT_5` | `aux_1` | Input only. |
| 6 | `INPUT_6` | `aux_2` | Input only. |
| 7 | `INPUT_7` | `aux_3` | Input only. |
| 8 | `INPUT_8` | `lid_switch` | Input only. Internal pull-up, active low. |

`Name` is a configurable label stored in runtime configuration. Puzzle solutions reference input indexes (example: `"3124"`).

### 3.2 Low-Power Outputs (4 channels)

| Function | Pin Name | Notes |
|----------|----------|-------|
| Output 1 | `AUX_OUT_1` | 3.3V logic, max ~12 mA. Can drive LED, MOSFET gate, relay coil via transistor. |
| Output 2 | `AUX_OUT_2` | Same. |
| Output 3 | `AUX_OUT_3` | Same. |
| Output 4 | `AUX_OUT_4` | Same. |

### 3.3 Audio Output

| Function | Pin Name | Notes |
|----------|----------|-------|
| Buzzer PWM | `PIEZO_PWM` | LEDC PWM output for piezo buzzer tone generation |

### 3.4 I2C Port

| Function | Pin Name | Notes |
|----------|----------|-------|
| SDA | `I2C_SDA` | |
| SCL | `I2C_SCL` | |
| 5V | — | From 5V rail via connector |
| GND | — | Common ground |

### 3.5 SPI Port

| Function | Pin Name | Notes |
|----------|----------|-------|
| MOSI | `SPI_MOSI` | |
| MISO | `SPI_MISO` | |
| SCLK | `SPI_SCLK` | |
| CS | `SPI_CS` | |
| 5V | — | From 5V rail via connector |
| GND | — | Common ground |

### 3.6 RS-485 Port (Option 2)

| Function | Pin Name | Notes |
|----------|----------|-------|
| TX | `RS485_TX` | UART TX to transceiver DI |
| RX | `RS485_RX` | UART RX from transceiver RO |
| Driver Enable | `RS485_DE` | Controls DE and ~RE (tied together) |

### 3.7 Status LED

| Function | Pin Name | Notes |
|----------|----------|-------|
| WS2812 data | `STATUS_LED` | Single RGB LED for status indication |

### 3.8 System / Reserved

| Function | Pin Name | Notes |
|----------|----------|-------|
| USB D+ | `USB_DP` | USB-Serial/JTAG (built-in) |
| USB D- | `USB_DM` | USB-Serial/JTAG (built-in) |
| Battery ADC | `BATT_SENSE` | Voltage divider from Vin (÷3 or ÷4 to stay under 3.3V) |

---

## 4. Peripheral Components

### 4.1 Countdown Display

| Parameter | Value |
|-----------|-------|
| Type | Adafruit 0.56 in 4-digit 7-segment LED display with HT16K33 I2C backpack (or equivalent HT16K33 module) |
| Interface | I2C |
| Driver IC | HT16K33 |
| Digit format | `MM:SS` with colon |
| Behavior | Countdown: `MM:SS` + 1 Hz colon blink; ready/lid-closed: blank; not-ready: `----` with progress bars; result: frozen time + 1 Hz blink for 120 s |
| Color | TBD (red, green, or blue — depends on theming) |
| Voltage | 5V (from I2C port) |
| Part number | Adafruit PID 878 (0.56" 4-digit 7-segment + HT16K33 backpack) |

### 4.2 Piezo Buzzer

| Parameter | Value |
|-----------|-------|
| Type | SunFounder Passive Buzzer Module |
| Drive | PWM square wave from ESP32 GPIO |
| Frequency range | 2-5 kHz recommended drive range |
| Voltage | 3-5V |
| Driver circuit | Module includes S8550 PNP transistor stage; verify final board drive/current in prototype tests |
| Part number | SunFounder Passive Buzzer Module (SKU TS0210D, per vendor listing) |
| Product page | https://www.sunfounder.com/products/passive-buzzer-module?_pos=2&_sid=7821e2f95&_ss=r |

### 4.3 Wire Connectors

| Parameter | Value |
|-----------|-------|
| Type | TBD — banana plugs, ring terminals, or bare wire into screw terminals |
| Detection method | Wire connects GPIO to GND. Disconnection = GPIO pulled high by internal pull-up. |
| Colors | 4 distinct colors (e.g., Red, Blue, Yellow, Green) |

### 4.4 Wiring Harness Connector

| Parameter | Value |
|-----------|-------|
| Type | TBD — JST-XH, Molex KK, or screw terminal block |
| Pin count | 12+ (8 GPIO + 5V + 3.3V + GND + GND) |
| Cable | TBD — ribbon cable or individual wires |

### 4.5 Lid / Tamper Switch

| Parameter | Value |
|-----------|-------|
| Type | Microswitch or magnetic reed switch |
| Wiring | NO (normally open) — closes when lid is closed, opens when lid is lifted |
| GPIO | `INPUT_8` (shared with wiring harness) |

Firmware lid gating modes for display blanking:

- `off`: ignore lid input
- `closed`: blank when lid GPIO is LOW
- `open`: blank when lid GPIO is HIGH

---

## 5. Connectors Summary

| Connector | Pins | Purpose |
|-----------|------|---------|
| Power input | 2 (V+, GND) | 6–12V DC battery supply |
| Wiring harness | 12+ | 8 GPIO + power + ground |
| I2C display | 4 (SDA, SCL, 5V, GND) | 7-segment countdown display |
| SPI display | 6 (MOSI, MISO, SCLK, CS, 5V, GND) | Future: RGB matrix |
| RS-485 | 3 logic + A/B/GND | Differential bus via external transceiver |
| Buzzer | 2 (signal, GND) | Piezo buzzer |
| USB-C | — | Programming and debug (built into DevKitC-1) |

---

## 6. Enclosure

| Parameter | Value |
|-----------|-------|
| Material | 3D-printed PLA or PETG |
| Design | TBD — depends on theming (bomb prop, junction box, etc.) |
| Mounting | TBD — Velcro, screws, or magnets |
| Access | Must allow USB-C access for emergency reflash |
| Files | Will be stored in `docs/hardware/` when ready |

---

## 7. Bill of Materials (BOM) — Prototype

| Qty | Component | Part Number | Notes | Est. Cost |
|-----|-----------|-------------|-------|-----------|
| 1 | ESP32-S3-DevKitC-1 v1.0 | — | Development board (prototype only) | ~$10 |
| 1 | Adafruit 0.56 in 4-digit 7-segment I2C display | PID 1002 (or equivalent) | HT16K33-based display module | ~$11 |
| 1 | SunFounder Passive Buzzer Module | TS0210D | Passive buzzer module, 3-5V, PWM-driven | ~$7 |
| 4 | Colored wire assemblies | — | Red, blue, yellow, green w/ connectors | ~$3 |
| 1 | Microswitch (lid) | — | NO type, lever actuator | ~$1 |
| 1 | Wiring harness connector | TBD | JST-XH or screw terminal | ~$2 |
| 1 | Battery holder / connector | — | For 6V–12V source | ~$3 |
| 1 | 5V voltage regulator | TBD | LDO or buck converter | ~$2 |
| 4 | NPN transistors (output drivers) | TBD | 2N2222 or similar | ~$1 |
| — | Resistors, caps, misc | — | Pull-ups, decoupling, voltage divider | ~$2 |
| 1 | Enclosure | — | 3D-printed | ~$3 |
| | | | **Total (prototype)** | **~$33** |

---

## 8. Datasheets

Store relevant datasheets in `docs/datasheets/`. Key ones to collect:

- [ ] ESP32-S3-WROOM-1 module datasheet
- [ ] ESP32-S3 technical reference manual
- [ ] Adafruit HT16K33 7-segment display docs/datasheet package
- [ ] SunFounder passive buzzer module technical datasheet (if published separately)
- [ ] Voltage regulator datasheet
- [ ] Wiring harness connector datasheet

Current references:

- Adafruit LED Backpack downloads (datasheets/schematic): https://learn.adafruit.com/adafruit-led-backpack/downloads
- SunFounder Passive Buzzer Module product page: https://www.sunfounder.com/products/passive-buzzer-module?_pos=2&_sid=7821e2f95&_ss=r
