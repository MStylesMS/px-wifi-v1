# PX-WiFi-V1 — Product Brief

**Product Name:** PX-WiFi-V1 Wireless Prop Controller  
**Manufacturer:** Paradox Productions  
**Version:** 1.0 Draft  
**Date:** 2026-04-07  

---

## Overview

The PX-WiFi-V1 is a compact, battery-powered wireless controller for escape room and haunted house props. It connects to the venue's existing WiFi network, enabling remote control, real-time monitoring, and instant reconfiguration — all without running dedicated cables to each prop.

Designed by escape room operators for escape room operators, the PX-WiFi-V1 replaces fragile Arduino-based one-off builds with a reliable, reusable platform that supports a wide range of puzzle types out of the box.

---

## Key Features

- **Wireless Control** — Connects to any standard WiFi network. Receives commands and reports events in real time via MQTT and WebSocket.
- **Battery Powered** — Operates on 6–12V battery power (rechargeable or disposable), enabling placement anywhere in the room without wall power.
- **Flexible I/O** — 8+ configurable GPIO channels via wiring harness. At least 4 channels can switch between input and output mode for maximum flexibility.
- **Display Support** — Dedicated I2C and SPI ports with 5V power rails for connecting 7-segment countdown displays, RGB LED matrices, and other indicators.
- **Audio Output** — Built-in DAC output for driving a piezoelectric buzzer or small speaker. Supports tones, beep patterns, and simple audio cues.
- **Low-Power Outputs** — 4+ dedicated output channels for triggering LEDs, relays, MOSFETs, solenoids, and other actuators.
- **Over-the-Air Updates** — Firmware can be updated wirelessly without opening the prop or connecting a cable.
- **Web Configuration** — Built-in web interface for on-site setup. No app required — just connect from any phone or laptop on the same network.
- **Real-Time Event Reporting** — Every player interaction (button press, wire pull, lid lift) is reported instantly to the game controller for scoring, automation, and live monitoring.

---

## Target Applications

| Puzzle Type | How PX-WiFi-V1 Supports It |
|-------------|---------------------------|
| Wire defusal / bomb prop | Monitor wire disconnect sequence, countdown timer, beeper |
| Code entry / keypad puzzles | Read button inputs, validate sequences, trigger locks |
| Physical interaction puzzles | Detect object placement, lever pulls, door openings |
| Lighting effects | Drive LED strips, spotlights, blacklight triggers |
| Audio cue props | Play tones, alarms, ambient effects via DAC output |
| Display props | Drive countdown timers, score displays, message boards |

---

## Integration

The PX-WiFi-V1 integrates with the Paradox Productions game controller ecosystem:

- **PxO (Orchestrator)** — Receives game state commands (start, stop, reset, pause)
- **PFx (Media Player)** — Coordinates with video and audio cue triggers
- **PxC (Clock)** — Syncs countdown timer with the room's master clock
- **Node-RED** — Compatible with standard MQTT wildcard subscriptions for custom automation

---

## Physical Specifications

| Parameter | Value |
|-----------|-------|
| Processor | ESP32-S3 (dual-core, 240 MHz, WiFi + BLE) |
| Power Input | 6–12V DC (barrel jack or screw terminal) |
| GPIO Channels | 8+ via wiring harness connector |
| Display Ports | 1× I2C (5V), 1× SPI (5V) |
| Audio Output | 1× DAC (3.5mm or header) |
| Low-Power Outputs | 4× (LED/relay/MOSFET drive) |
| Wireless | WiFi 802.11 b/g/n (2.4 GHz) |
| Dimensions | TBD (target: credit-card size) |
| Enclosure | Custom 3D-printed case (design TBD) |

---

## Ordering & Availability

*Not yet available. Currently in prototype phase.*
