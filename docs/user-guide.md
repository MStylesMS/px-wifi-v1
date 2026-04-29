# PX-WiFi-V1 — User Guide

**Version:** 1.0  
**Last Updated:** 2026-04-13  

Welcome to the PX-WiFi-V1 Wireless Prop Controller. This guide walks you through setup, configuration, and operation of your escape room prop.

---

## Table of Contents

1. [What is PX-WiFi-V1?](#what-is-px-wifi-v1)
2. [First Time Setup](#first-time-setup)
3. [Understanding the Display](#understanding-the-display)
4. [Connecting to WiFi](#connecting-to-wifi)
5. [Configuring Your Puzzle](#configuring-your-puzzle)
6. [Testing & Troubleshooting](#testing--troubleshooting)
7. [Daily Operation](#daily-operation)

---

## What is PX-WiFi-V1?

The PX-WiFi-V1 is a wireless puzzle controller that helps manage escape room props. It:

- Connects props to your WiFi network for remote control
- Displays timers and status on a built-in 7-segment LED screen
- Detects player actions (wires pulled, buttons pressed, lids opened)
- Reports everything to your game controller for scoring and automation
- Runs on battery power, so no cables needed

**Primary Use (v1.0):** Wire defusal puzzles — players disconnect wires in the correct sequence before a countdown timer expires.

---

## First Time Setup

### Step 1: Unpack & Inspect

You should have:

- PX-WiFi-V1 main unit
- Wiring harness with 4 colored wires (red, green, yellow, blue)
- USB power cable or battery connector (model-dependent)
- 7-segment LED display (external, connected via I2C)
- Small piezoelectric buzzer (for beeps and tones)

### Step 2: Power On

Connect 6–12V power to the barrel jack or screw terminal marked "PWR". The device will boot within 5 seconds. You should see:

1. A brief blue pulse on the onboard RGB LED (WiFi connecting)
2. The 7-segment display LED dots blinking (WiFi strength indicator)

### Step 3: Connect to WiFi (First Time)

If the device is not yet configured for the venue WiFi:

1. On your phone or laptop, look for a WiFi network named **`Paradox-px-wifi-v1-XXXX`** (where XXXX is a unique ID)
2. Connect to this temporary AP network (no password required)
3. Open a web browser and navigate to **`http://192.168.4.1`**
4. You'll see the **Configuration Page**

### Step 4: Configure Network

In the **Network** section:

1. **WiFi SSID** — Select your venue's WiFi network from the scan list
2. **WiFi Password** — Enter the venue WiFi password
3. **MQTT Broker Address** — Enter the IP or hostname of your MQTT broker (or leave blank if using local mode)
4. **MQTT Port** — Default is 1883 (leave as is)
5. Click **Save**

Important WiFi notes:

- The PX-WiFi-V1 supports **2.4 GHz WiFi only**. If your venue uses the same SSID on 2.4 GHz and 5 GHz, make sure 2.4 GHz is enabled.
- Password-free networks are supported, but **enterprise WiFi** networks that require usernames, certificates, captive portals, or 802.1X login are **not** supported from the prop UI.
- If the connection fails, the Connection page now shows the last WiFi error reported by the ESP32 so you can tell the difference between bad credentials and AP compatibility problems.

The device will reboot and attempt to connect to the venue WiFi. The RGB LED will transition:

- Blue pulse → Connecting to venue WiFi
- Cyan double-blink → Connecting to MQTT broker
- Solid green → Online and ready

---

## Understanding the Display

The 7-segment display shows different information based on what the prop is doing:

| State | Display Shows | What It Means |
|-------|--------------|---------------|
| **READY** | 1–4 blinking dots | WiFi signal strength. More dots = stronger signal. Dots blink once per second. |Only 1 dot means no connection or too week to use.
| **NOT_READY** | `----` with top/bottom bars | Wires are not all connected. Bars show which wires are missing. |
| **COUNTDOWN** | `MM:SS` with blinking `:` | Timer counting down. Colon blinks 1 Hz. |
| **PAUSED** | `MM:SS` with blinking `:` | Timer paused (stopped but still showing time). Colon blinks. |
| **DEFUSED** | `MM:SS` with solid `:` | Success! Time frozen on display for 2 minutes. Colon stays solid. |
| **DETONATED** | `MM:SS` or blank | Failure! Time frozen for 2 minutes, then display blanks automatically. |

### WiFi Strength Indicator (READY state)

When the prop is ready and waiting for a game:

- **1 dot** — No WiFi or signal very weak. May have latency issues.
- **2 dots** — Weak signal. Commands may be delayed up to 1 second.
- **3 dots** — Medium signal. Good for most escape rooms (typical distance 10–20 meters).
- **4 dots** — Strong signal. Optimal performance.

**If you see only 1 dot:** Move the prop closer to your AP, or check WiFi credentials.

---

## Connecting to WiFi

### Automatic (Recommended)

The device attempts automatic WiFi connection:

1. On first boot without a known network, it creates an AP (access point) with SSID `Paradox-px-wifi-v1-XXXX`
2. Connect to this AP from your phone
3. Open `http://192.168.4.1` and enter your venue WiFi details
4. Device connects to venue WiFi and becomes a WiFi client

If the selected SSID advertises a supported security mode, the prop now uses the scanned network's security settings when joining instead of assuming a generic WPA2 configuration.

### Manual AP Mode

If you need to reconnect to WiFi:

1. Hold the **Reset** button (or power cycle 3 times rapidly) to force AP mode
2. Connect to the temporary AP from your phone
3. Open `http://192.168.4.1`
4. Update WiFi settings as needed

---

## Configuring Your Puzzle

### Basic Settings

Navigate to the **Configuration Page** at `http://<device-ip>/config.html`:

#### Identity Section
- **Site Name** — Name of your venue (e.g., "Downtown Escape")
- **Zone Name** — Location within venue (e.g., "Room 3")
- **Device ID** — Unique identifier for this prop

#### Puzzle Section
- **Default Countdown Time** — How long the timer runs (in seconds). Default: 3600 (60 minutes). For a 5-minute puzzle, enter `300`.
- **Wire Count** — How many wires must be disconnected (1–4 wires supported in v1)
- **Wire Sequence** — Order players must pull wires (default: `1234`). For sequence red→blue→green→yellow, enter `1423`.
- **Penalty Amount** — Seconds deducted for wrong wire (default: 30)
- **Max Tries** — Number of wrong wires before forced failure (default: 3)

#### Save
Click **Save** to store these settings persistently. Settings survive power loss.

### Advanced Manual Config (Not in Web UI)

Some advanced options are only available by editing SPIFFS config directly.

Config file path: `/spiffs/config.json`

- `lowBatteryCutoffPercent` (default: `20`)
- Range: `0` to `100`
- Behavior: if battery percentage stays at or below this value for 15 seconds continuously, the device enters deep sleep
- Set to `0` to disable this feature
- Wake behavior after low-battery deep sleep: the next state change on the red wire input (GPIO4) wakes the unit. Reset button and power-cycle also wake/restart the unit.

Example:

```json
{
  "lowBatteryCutoffPercent": 20
}
```

---

## Testing & Troubleshooting

### Test the Wires

1. In READY state, disconnect each wire one at a time
2. Display should change from `----` to show which wires are missing
3. Reconnect all wires → should return to READY

### Test the Timer

1. In Configuration, set **Default Countdown Time** to a short value (e.g., `30` for 30 seconds)
2. From the Live Status page or via MQTT, send `{"command":"start"}`
3. Timer should count down; display shows `MM:SS`
4. Listen for beeps at 5 minutes, 1 minute, and final 10 seconds

### WiFi Issues

| Symptom | Cause | Solution |
|---------|-------|----------|
| Only 1 dot showing | No WiFi or weak signal | Move prop closer to AP. Check WiFi SSID/password in config. |
| Commands delayed >1 sec | Weak WiFi signal | Reposition prop. Check for RF interference (microwaves, radios). |
| Can't reach config page | Device not on WiFi yet | Look for `Paradox-px-wifi-v1-XXXX` AP. If not visible, power cycle device. |
| RGB LED stays blue (pulsing) | WiFi not connecting | Check the Connection page for the last WiFi error. Verify the venue SSID has 2.4 GHz enabled and is not an enterprise/captive-portal network. |
| Network is visible but connection fails repeatedly | Unsupported AP security mode or AP requires a password the prop did not receive | Re-scan the SSID, confirm the password, and check the reported WiFi error. WPA/WPA2/WPA3 personal networks are supported. Enterprise networks are not. |

### Power Issues

| Symptom | Cause | Solution |
|---------|-------|----------|
| Display doesn't light up | No power | Check battery voltage (6–12V). Verify connector polarity. |
| Frequent disconnects | Weak power supply | Use higher-capacity battery or main power. Add 100µF capacitor across VCC/GND of display backpack. |

---

## Daily Operation

### Before Players Arrive

1. **Power on the prop** and wait for the RGB LED to turn solid green
2. **Verify 3–4 WiFi dots** on the display (check signal strength)
3. **Connect all wires** in the correct sequence
4. **Test a countdown** via web UI or MQTT (`start` command, wait 10 seconds, then `reset`)
5. **Verify audio** — buzzer should beep on countdown milestones

### During Game

1. **Player briefs** on the puzzle
2. **Game controller sends `start`** → Timer begins, wire monitoring active
3. **Players pull wires** in sequence → Beeps on correct wire, buzz on wrong wire
4. **Success:** All wires correct before timeout → Display shows `MM:SS` with solid colon, success beep plays
5. **Failure:** Timeout or wrong wire → Display shows final time, failure beep plays, display blanks after 2 minutes

### Between Games

1. **Wait for auto reset** (2 minutes after success/failure) or manually send `reset` command
2. **Verify all wires reconnected** (display returns to `----`)
3. **System is ready** for next game

---

## Connecting to Game Controller (MQTT)

If using PxO (Paradox Orchestrator) or another MQTT-based game engine:

### MQTT Commands

Send JSON payloads to `paradox/{site}/{zone}/commands`:

```json
{"command": "start", "time": 300}      // Start 5-minute countdown
{"command": "pause"}                    // Pause timer
{"command": "resume"}                   // Resume timer
{"command": "reset"}                    // Return to READY/NOT_READY
{"command": "setTime", "time": 180}     // Set time to 180 seconds
{"command": "solve"}                    // Force DEFUSED (success)
{"command": "fail"}                     // Force DETONATED (failure)
{"command": "getState"}                 // Request immediate state report
```

### Monitoring State

The device publishes real-time state to `paradox/{site}/{zone}/state`:

```json
{
  "status": "online",
  "gameState": "countdown",
  "readyState": "ready",
  "timeRemaining": 142,
  "wiresDisconnected": [1, 2],
  "correctSoFar": true
}
```

### Events

Player actions are published to `paradox/{site}/{zone}/events`:

```json
{"event": "wireDisconnected", "data": {"wire": 1, "correct": true}}
{"event": "defused", "data": {"timeRemaining": 42}}
{"event": "detonated", "data": {"reason": "timeout"}}
```

---

## Technical Specifications

| Parameter | Value |
|-----------|-------|
| Processor | ESP32-S3 (240 MHz dual-core) |
| WiFi | 802.11 b/g/n (2.4 GHz) |
| Range | 30 meters typical (through 1–2 walls) |
| Battery Life | 8+ hours in READY mode (light sleep) |
| Command Latency | <250 ms typical, <1 sec worst-case |
| Display | 7-segment, 4-digit, I2C connected |
| Audio | PWM buzzer output, MML-format melody support |
| Time Accuracy | ±1% drift (drift corrected via keep-sync if enabled) |

---

## Support & Resources

**Documentation:**
- Functional Spec: [functional-spec.md](functional-spec.md)
- Hardware Spec: [hardware-spec.md](hardware-spec.md)
- MQTT Protocol: [../PR_PX_APP_COMM_UPDATE.md](../PR_PX_APP_COMM_UPDATE.md)

**Contact Paradox Support:**
- Email: support@paradox-productions.local (TBD)
- Slack: #prop-wifi-v1 (TBD)

---

**Happy puzzling!** 🎮
