# PX-WiFi-V1 — Functional Specification

**Version:** 0.1 Draft  
**Date:** 2026-04-07  
**Status:** DRAFT — Brain dump, needs review  

---

## 1. Purpose

This document specifies **what** the PX-WiFi-V1 does from the user's and game controller's perspective. It does not specify implementation details (hardware selection, software architecture, or code design). Those emerge from this spec during development.

---

## 2. Device Roles

The PX-WiFi-V1 serves two roles:

1. **Generic Prop Controller** — A reusable platform with configurable I/O that can be adapted to many puzzle types via firmware configuration and wiring harness changes.
2. **First Puzzle: Wire Defusal** — The v1.0 firmware implements a specific puzzle where players must disconnect wires in the correct sequence while a countdown timer runs.

---

## 3. System Context

```
┌─────────────┐     WiFi/MQTT      ┌──────────────────┐
│  PxO Game    │◄──────────────────►│   PX-WiFi-V1     │
│  Controller  │                    │   (this device)   │
└──────┬───────┘                    └────────┬─────────┘
       │                                     │ Wiring Harness
       │                              ┌──────┴──────┐
  ┌────┴────┐                    ┌────┴─┐  ┌───┐  ┌─┴──┐
  │ PxC     │                    │Wires │  │Buz│  │Disp│
  │ Clock   │                    │(4)   │  │zer│  │lay │
  └─────────┘                    └──────┘  └───┘  └────┘
```

---

## 4. Power & Connectivity

### 4.1 Power

| Requirement | Detail |
|-------------|--------|
| Input voltage | 6–12V DC |
| Connector | TBD (barrel jack, screw terminal, or JST) |
| Battery operation | Must run for minimum 4 hours on a single battery charge/set |
| Power indicator | Status LED indicates power-on and low battery |
| Regulation | Onboard regulators provide 5V (peripherals) and 3.3V (ESP32) |

### 4.2 WiFi

| Requirement | Detail |
|-------------|--------|
| Mode | Station mode (connects to venue AP) |
| Fallback | If connection fails after 30s, enters AP mode for configuration |
| AP SSID | `Paradox-<device-id>` (e.g., `Paradox-wire-defusal`) |
| Reconnection | Automatic reconnection with exponential backoff |
| Signal | Must operate reliably at typical escape room distances (≤30m through walls) |

### 4.3 Communication Protocols

| Protocol | Use |
|----------|-----|
| MQTT | Primary command/event channel to game controller |
| WebSocket | Live web UI updates, alternative to MQTT for local control |
| HTTP | Configuration web interface, REST API |

---

## 5. I/O Requirements

### 5.1 General-Purpose I/O (Wiring Harness)

| Requirement | Detail |
|-------------|--------|
| Minimum inputs | 8 digital inputs |
| Switchable I/O | At least 4 of the 8 must be configurable as outputs |
| Connector | Multi-pin wiring harness connector (e.g., JST-XH, Molex) |
| Input type | Active-low with internal pull-up (button/switch to ground) |
| Debouncing | Software debounce, configurable per-channel (default 50ms) |
| Output type | 3.3V logic level, max ~12mA per pin |

### 5.2 Low-Power Outputs

| Requirement | Detail |
|-------------|--------|
| Count | Minimum 4 dedicated output channels |
| Drive capability | Sufficient to trigger: indicator LEDs, small relays, logic-level MOSFETs |
| Control | On/off, PWM (for LED dimming), or pulse (for momentary triggers) |

### 5.3 Audio Output

| Requirement | Detail |
|-------------|--------|
| Type | DAC output or PWM-based tone generation |
| v1.0 use | Drive a piezoelectric buzzer for countdown beeps and alerts |
| Future | Support for I2S audio output to external amplifier/speaker |
| Capabilities | Configurable tone frequency, duration, and pattern (e.g., beep-beep-beep) |

### 5.4 I2C Port

| Requirement | Detail |
|-------------|--------|
| Count | Minimum 1 |
| Power | Must provide 5V and GND on the connector |
| v1.0 use | 7-segment LED display for countdown timer |
| Connector | 4-pin header or JST (SDA, SCL, 5V, GND) |

### 5.5 SPI Port

| Requirement | Detail |
|-------------|--------|
| Count | Minimum 1 |
| Power | Must provide 5V and GND on the connector |
| Future use | RGB matrix display, LED panel |
| Connector | 6-pin header or JST (MOSI, MISO, SCLK, CS, 5V, GND) |

### 5.6 Status Indicator

| Requirement | Detail |
|-------------|--------|
| Type | Onboard RGB LED (WS2812 or similar) |
| Patterns | See Section 9: LED Status Indicator |

---

## 6. Wire Defusal Puzzle (v1.0 Firmware)

### 6.1 Overview

Players encounter a prop (themed as a bomb, security panel, junction box, etc.) with 4 colored wires and a countdown display. They must disconnect the wires in the correct order before time runs out. Disconnecting a wire in the wrong order triggers a penalty (configurable: beep, time deduction, or instant failure).

### 6.2 Physical Setup

- **4 wires** connected to 4 GPIO inputs via the wiring harness
- **Lid/tamper switch** on 1 GPIO input (detects when players first interact with the prop)
- **Countdown display** on I2C port (7-segment, 4-digit: MM:SS)
- **Piezo buzzer** on DAC/PWM output
- **Status RGB LED** onboard

### 6.3 Game States

```
                MQTT "start"
  [IDLE] ──────────────────► [ARMED]
    ▲                           │
    │ MQTT "reset"              │ Lid opened
    │                           ▼
    │                      [COUNTDOWN]
    │                        /     \
    │            All wires  /       \ Timer hits 0
    │           correct    /         \ or wrong order
    │                     ▼           ▼
    │               [DEFUSED]    [DETONATED]
    │                   │             │
    └───────────────────┴─────────────┘
                  MQTT "reset"
```

| State | Description |
|-------|-------------|
| **IDLE** | Waiting for game start. Display blank or showing `--:--`. No inputs monitored. |
| **ARMED** | Game started. Display shows initial time. Waiting for lid open or first wire pull. |
| **COUNTDOWN** | Timer actively counting down. Wire disconnects are monitored and validated. |
| **DEFUSED** | All wires disconnected in correct order. Timer stops. Success indication. |
| **DETONATED** | Timer expired or wrong wire order. Failure indication. |

### 6.4 Timer Behavior

| Requirement | Detail |
|-------------|--------|
| Internal countdown | Device runs its own countdown timer independently |
| Time sync | Game controller may push time updates via MQTT (`setTime` command) |
| Display update rate | At least 1 Hz (every second) |
| Initial time | Configurable via MQTT command or web config (default: 5:00) |
| Beep pattern | Configurable. Example: beep every second in last 30s, continuous in last 10s |

### 6.5 Wire Sequence Validation

| Requirement | Detail |
|-------------|--------|
| Correct order | Configurable sequence (e.g., [Red, Blue, Yellow, Green]) |
| Wrong wire penalty | Configurable: `beep_only`, `time_penalty`, `instant_fail` |
| Time penalty amount | Configurable (default: 30 seconds deducted) |
| Detection | Each wire GPIO transitions from connected (LOW) to disconnected (HIGH) |
| Debounce | 50ms minimum to avoid false triggers from vibration |

### 6.6 Audio Feedback

| Event | Audio Response |
|-------|---------------|
| Lid opened | Single short beep |
| Correct wire disconnected | Rising tone (pitch increases with each correct wire) |
| Wrong wire disconnected | Harsh buzz / error tone |
| Last 30 seconds | Periodic beep (1 Hz) |
| Last 10 seconds | Rapid beep (4 Hz) |
| Defused (success) | Victory melody or sustained tone |
| Detonated (failure) | Descending tone / alarm |
| MQTT command received | Optional brief acknowledgment beep |

### 6.7 Display Behavior

| State | Display Shows |
|-------|--------------|
| IDLE | `--:--` or blank |
| ARMED | Initial countdown time (e.g., `05:00`) |
| COUNTDOWN | Live countdown `MM:SS` |
| DEFUSED | `00:00` or remaining time, then flashing |
| DETONATED | `00:00` flashing, or `FAIL` if display supports it |

---

## 7. MQTT Interface

All communication follows the **Paradox v2 MQTT Protocol** (see `PR_PX_APP_COMM_UPDATE.md`).

**Base topic:** `paradox/{site}/{zone}/`  
**Default zone:** `wire-defusal` (configurable)

### 7.1 Inbound Commands (subscribe to `.../commands`)

| Command | Payload | Description |
|---------|---------|-------------|
| `start` | `{"command": "start"}` | Begin the game. Transition to ARMED state. |
| `start` | `{"command": "start", "time": 300}` | Begin with specific countdown (seconds). |
| `stop` | `{"command": "stop"}` | Halt countdown immediately. Remain in current state. |
| `pause` | `{"command": "pause"}` | Pause countdown. Timer holds. Display blinks. |
| `resume` | `{"command": "resume"}` | Resume countdown from paused state. |
| `reset` | `{"command": "reset"}` | Return to IDLE. Clear all state. |
| `setTime` | `{"command": "setTime", "time": 180}` | Update countdown to specific value (seconds). |
| `setSequence` | `{"command": "setSequence", "order": [3,1,4,2]}` | Set the correct wire disconnect order (wire numbers). |
| `setPenalty` | `{"command": "setPenalty", "mode": "time_penalty", "amount": 30}` | Configure wrong-wire penalty behavior. |
| `getState` | `{"command": "getState"}` | Trigger immediate state report. |
| `restart` | `{"command": "restart"}` | Soft-reboot the device. |
| `identify` | `{"command": "identify"}` | Flash the RGB LED for physical identification. |

### 7.2 Outbound State (publish to `.../state`, retained)

```json
{
  "ts": 1700000000000,
  "status": "online",
  "id": "wire-defusal",
  "gameState": "countdown",
  "timeRemaining": 142,
  "wiresDisconnected": [3, 1],
  "wiresRemaining": [4, 2],
  "correctSoFar": true,
  "penalty": "time_penalty",
  "uptime": 3600,
  "version": "1.0.0",
  "ip": "192.168.1.50",
  "rssi": -42
}
```

### 7.3 Outbound Events (publish to `.../events`)

| Event | Payload | When |
|-------|---------|------|
| `lidOpened` | `{"event": "lidOpened", "ts": ...}` | Lid/tamper switch triggered |
| `wireDisconnected` | `{"event": "wireDisconnected", "ts": ..., "data": {"wire": 3, "position": 1, "correct": true}}` | A wire is pulled |
| `defused` | `{"event": "defused", "ts": ..., "data": {"timeRemaining": 42}}` | All wires correct |
| `detonated` | `{"event": "detonated", "ts": ..., "data": {"reason": "timeout"}}` | Timer expired |
| `detonated` | `{"event": "detonated", "ts": ..., "data": {"reason": "wrongWire", "wire": 2}}` | Wrong wire pulled (instant_fail mode) |
| `penaltyApplied` | `{"event": "penaltyApplied", "ts": ..., "data": {"wire": 2, "penalty": 30, "newTime": 112}}` | Time deducted for wrong wire |
| `stateChanged` | `{"event": "stateChanged", "ts": ..., "data": {"from": "idle", "to": "armed"}}` | Any game state transition |
| `timerSync` | `{"event": "timerSync", "ts": ..., "data": {"time": 142}}` | Periodic time broadcast (every 10s during countdown) |

### 7.4 Outbound Warnings (publish to `.../warnings`)

| Warning | When |
|---------|------|
| `{"warning": "lowBattery", "ts": ..., "message": "Battery voltage below 6.5V", "data": {"voltage": 6.3}}` | Battery getting low |
| `{"warning": "wifiReconnect", "ts": ..., "message": "WiFi connection lost, reconnecting"}` | WiFi dropped |
| `{"warning": "mqttReconnect", "ts": ..., "message": "MQTT broker connection lost"}` | MQTT dropped |

---

## 8. Web Interface

### 8.1 Configuration Page (`http://<device-ip>/`)

| Section | Fields |
|---------|--------|
| **Network** | WiFi SSID, Password, MQTT broker address, MQTT port |
| **Identity** | Site name, Zone name, Device ID |
| **Puzzle** | Default countdown time, Wire sequence, Penalty mode, Penalty amount |
| **Audio** | Beep volume, Enable/disable audio feedback per event |
| **System** | Firmware version, Uptime, Free heap, OTA update URL + trigger |

### 8.2 Live Status (`ws://<device-ip>/ws`)

WebSocket pushes the same JSON events as MQTT in real time. Useful for a technician standing next to the prop with a phone — no MQTT broker needed for local debugging.

---

## 9. LED Status Indicator

| Color | Pattern | Meaning |
|-------|---------|---------|
| Magenta | Solid | AP mode — waiting for WiFi configuration |
| Blue | Slow pulse (1 Hz) | Connecting to WiFi |
| Cyan | Double blink | Connected to WiFi, connecting to MQTT |
| Green | Solid | Online and idle (IDLE state) |
| Green | Breathing | Armed, waiting for player interaction |
| White | Slow pulse | Countdown running |
| Yellow | Fast blink (4 Hz) | Paused |
| Red | Solid (3 seconds) | Wrong wire / penalty |
| Red | Fast blink | Detonated (failure) |
| Green | Fast blink | Defused (success) |
| Orange | Double blink | OTA update in progress |

---

## 10. Configuration Defaults

| Parameter | Default | Configurable Via |
|-----------|---------|-----------------|
| Countdown time | 300 seconds (5:00) | MQTT, Web UI |
| Wire sequence | [1, 2, 3, 4] | MQTT, Web UI |
| Penalty mode | `time_penalty` | MQTT, Web UI |
| Penalty amount | 30 seconds | MQTT, Web UI |
| Timer sync interval | 10 seconds | Web UI |
| Beep volume | 80% | Web UI |
| WiFi AP timeout | 30 seconds | Web UI |
| Heartbeat interval | 30 seconds | Web UI |

---

## 11. Open Questions

<!-- Mark: dump anything you're unsure about here. We'll resolve these before finalizing. -->

- [ ] Should the countdown timer display show tenths of a second in the final 60s?
- [ ] Should there be a "hint" mode where the display flashes the next correct wire color?
- [ ] How should the prop behave if WiFi drops mid-game? (Proposal: continue locally, buffer events, replay when reconnected)
- [ ] Should the prop support multiple puzzle modes beyond wire defusal in v1.0, or keep it single-purpose?
- [ ] What is the minimum battery life requirement? 4 hours? 8 hours? Full day?
- [ ] Should the lid switch be required, or optional? (Some theming may not have a lid)
- [ ] Should wires be re-connectable? (i.e., can a player plug a wire back in and try again?)
- [ ] Maximum number of wires? Is 4 always enough, or should it support 6–8?
