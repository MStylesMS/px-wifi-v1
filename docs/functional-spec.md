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
| Backoff policy | Retry delays: 1s, 2s, 4s, 8s, 16s, 32s, then capped at 32s between attempts |
| Backoff reset | On successful reconnect, next failure restarts backoff at 1s |
| Signal | Must operate reliably at typical escape room distances (≤30m through walls) |

### 4.3 Communication Protocols

| Protocol | Use |
|----------|-----|
| MQTT | Primary command/event channel to game controller |
| WebSocket | Live web UI updates, alternative to MQTT for local control |
| HTTP | Configuration web interface, REST API |

MQTT reconnect uses exponential backoff independent of WiFi: 1s, 2s, 4s, 8s, 16s, 32s, then capped at 32s maximum wait between attempts.

### 4.4 Power Saving and Wake Strategy

Power saving is **enabled by default** using light sleep mode.

| Option | Behavior | Status |
|--------|----------|--------|
| Light sleep | MCU sleeps between periodic tasks, keeps RAM/peripherals and WiFi available. GPIO interrupts wake on wire disconnect. | **IMPLEMENTED** |
| Display blanking | In `READY`, all segments are off except WiFi indicator dots. | Implicit in WiFi display mode |
| Wake on GPIO | Wire GPIO inputs trigger MCU wake from light sleep on HIGH edge (wire disconnect). | **IMPLEMENTED** |

**Light Sleep Details:**

- **Enabled in READY state:** MCU enters light sleep with display refresh rate reduced to 1 Hz.
- **Enabled in NOT_READY state:** MCU enters light sleep (display refreshes every 100 ms to show wire progress).
- **Disabled during COUNTDOWN/PAUSED/DEFUSED/DETONATED:** MCU stays awake for responsive gameplay.
- **Wake sources:** 
  - GPIO edge on wire inputs (disconnect detected)
  - Periodic task wake (1 Hz display updates in READY, 100 ms in other states)
  - MQTT command arrival
- **Power savings:** Approximately **42%** reduction in total system power draw during idle periods with normal WiFi signal.

WiFi connectivity remains active during light sleep, enabling responsive MQTT command reception with <250 ms typical latency.

Command latency targets met:

- Wake latency for inbound MQTT command processing: typically <250 ms.
- Worst-case practical latency: <1 second.

If measured latency exceeds 1 second in venue conditions, light sleep can be disabled via configuration.

Deep-sleep mode is not implemented in v1.0.

---

## 5. I/O Requirements

### 5.1 General-Purpose I/O (Wiring Harness)

| Requirement | Detail |
|-------------|--------|
| Minimum inputs | 8 digital inputs (v1 uses 4 wires + optional lid) |
| Switchable I/O | At least 4 of the 8 must be configurable as outputs |
| Connector | Multi-pin wiring harness connector (e.g., JST-XH, Molex) |
| Input type | Active-low with internal pull-up (button/switch to ground) |
| Debouncing | Software debounce, configurable by check interval + consecutive matching reads |
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
| Type | PWM-based tone generation to passive piezoelectric buzzer |
| v1.0 use | Drive passive piezo buzzer for countdown beeps and alerts |
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
- **Optional lid/tamper switch** on 1 GPIO input
- **Countdown display** on I2C port (7-segment, 4-digit: MM:SS)
- **Passive piezo buzzer** on PWM output
- **Status RGB LED** onboard

Lid mode is configurable as:

- `off` — ignore lid input for display gating
- `closed` — lid considered closed when GPIO level is LOW
- `open` — lid considered closed when GPIO level is HIGH

When lid is considered closed, the external 7-segment display is blanked.

### 6.3 Game States

```
           MQTT "start" or "resume" (optional time)
  [READY / NOT_READY] ───────────────────────────────► [COUNTDOWN]
        ▲                                                     │
        │ MQTT "reset"                                       │
        │                                                     │
        │                                           timer = 0 │
        │                                        or fail mode │
        │                                                     ▼
        │                                                [DETONATED]
        │                                                     │
        │                                                     │ hold up to 5 min
        │                                                     │ then auto reset
        │                                                     │
        │                                                     ▼
        │                                             [READY / NOT_READY]
        │
        │ all required wires disconnected in order
        └───────────────────────────────────────────── [DEFUSED]
                                                              │
                                                              │ hold up to 5 min
                                                              │ then auto reset
                                                              ▼
                                                      [READY / NOT_READY]
```

| State | Description |
|-------|-------------|
| **NOT_READY** | One or more required wires are not connected. Display shows per-wire readiness markers in color order (R, G, Y, B). |
| **READY** | All required wires are connected. Device is ready to start countdown. |
| **COUNTDOWN** | Timer actively counting down. Wire disconnects are monitored and validated. |
| **DEFUSED** | Required disconnect sequence completed. Timer stops and remains frozen for up to 5 minutes or until `reset`. |
| **DETONATED** | Timer expired or failure condition triggered. Timer remains frozen for up to 5 minutes or until `reset`. |

### 6.4 Timer Behavior

| Requirement | Detail |
|-------------|--------|
| Internal countdown | Device runs its own countdown timer independently |
| Start behavior | `start` and `resume` both begin countdown immediately from current time (no lid wait) |
| Combined command | `start` may include `time` in the same payload, e.g. `{"command":"start","time":900}` |
| Time sync | Game controller may push time updates via MQTT (`setTime` command) |
| Display update rate | At least 1 Hz (every second) |
| Default initial time | 3600 seconds (60:00) unless changed by saved configuration |
| Hold-after-end | On `DEFUSED` or `DETONATED`, hold displayed final time up to 5 minutes, then auto-reset |
| No tenths display | 4-digit display remains `MM:SS` only; no tenths shown |

Display-specific behavior for HT16K33 4-digit module:

- During `COUNTDOWN` and `PAUSED`, show `MM:SS` with colon blinking at 1 Hz.
- In `DEFUSED` or `DETONATED`, freeze the final displayed time and continue 1 Hz colon blink for 120 seconds (or until reset), then blank.
- In `READY`, show **WiFi signal strength indicator**: 1–4 dots (decimal points) on digits 0–3, blinking at 1 Hz.
  - 1 dot: No WiFi connection or very poor signal (<−90 dBm)
  - 2 dots: Weak WiFi signal (−80 to −90 dBm)
  - 3 dots: Medium WiFi signal (−60 to −80 dBm)
  - 4 dots: Strong WiFi signal (>−60 dBm)
- In `NOT_READY`, show `----` as base state with wire progress bars overlaid.
- In `NOT_READY`, overlay wire progress bars:
  - wires 1–4 map to top bars (`A` segments) of digits 1–4
  - wires 5–8 map to bottom bars (`D` segments) of digits 1–4
  - ignored wires (indexes above `wireCount`) remain as `-` (treated like closed/unused)

### 6.5 Wire Sequence Validation

| Requirement | Detail |
|-------------|--------|
| Supported wire count | v1 supports 4 wires; architecture must support up to 8 wires in future |
| Input identity model | Inputs are indexed and named separately: `INPUT_1..INPUT_8` + user label (`Name`) |
| Default input names | `INPUT_1=red`, `INPUT_2=green`, `INPUT_3=yellow`, `INPUT_4=blue`, `INPUT_5=white`, `INPUT_6=orange`, `INPUT_7=brown`, `INPUT_8=purple` |
| Correct order | Configurable disconnect order via solution vector string (default `"1234"`); only digits `1-8` are allowed, max length 8 |
| Required sequence length | Derived automatically from `solution` vector length |
| Wrong wire mode | Configurable via `setMode`: `buzz`, `penalty`, `instant` |
| Time penalty amount | Configurable (default: 30 seconds deducted) |
| Penalty edge handling | If remaining time <30s: detonate immediately. If 30-60s: set remaining time to 20s |
| Max tries | Integer 1-100 (inclusive), applies in `buzz` and `penalty` modes |
| Detection | Each wire GPIO transitions from connected (LOW) to disconnected (HIGH) |
| Reconnect handling | Reconnection rules apply only in `buzz` and `penalty` modes |
| Debounce | Default check interval 10ms and 5 consecutive identical readings |

Example: with 4 wires and custom order red/yellow/green/blue, solution vector is `"1324"`.

### 6.6 Audio Feedback

| Event | Audio Response |
|-------|---------------|
| Countdown, lid closed, >5 min left | 1 second beep once per minute |
| Countdown, lid open OR <=5 min left | 0.1 second beep once per second |
| Last 60 seconds | 0.1 second beep at 1 Hz (short beep every second) |
| Last 10 seconds | 0.1 second beep every 250ms |
| Correct wire disconnected | Rising tone (pitch increases with each correct wire) |
| Wrong wire disconnected | Harsh buzz / error tone |
| Defused (success) | Configurable: `none`, `beeps` (3 quick high beeps), or `melody` |
| Detonated (failure) | Configurable: `none`, `buzz` (5s at 1kHz), or `melody` |
| Detonated active sound | Solid tone/buzz for 5 seconds on detonation |
| WiFi connected | Medium-high long beep followed by 1–4 short beeps (matching current WiFi signal bars) |
| WiFi lost | Medium-low long beep |
| Low battery alert | Very low-pitched triple beep; repeats once every 5 minutes while battery is below `lowBatteryPercent` |
| Shutdown (before deep sleep) | Very low-pitched long beep; plays during ~1.4-second countdown before entering sleep mode |
| MQTT command received | Optional brief acknowledgment beep |

Melody strings use **MML-style** (Music Macro Language) notation: note + octave + duration, comma-separated.   
Example: `C5-4,D5-4,E5-4` = C, D, E in octave 5, each quarter-note duration.

#### 6.6.1 Default Melodies

**Success (defused) melody:**
```
C6-8,D6-8,E6-4
```
(High three-note ascending tone sequence)

**Failure (detonated) melody:**
```
E5-4,D5-4,C5-4,B4-4
```
(Descending tone sequence suggesting failure/alarm)

Both are configurable via the web UI `Audio` configuration section.

### 6.7 Display Behavior

| State | Display Shows |
|-------|--------------|
| READY | Current configured start time, with center `:` blinking 0.1s once per second |
| NOT_READY | Four-character readiness string in index order `INPUT_1..INPUT_4` (`-` for connected, input index digit for disconnected) |
| COUNTDOWN | Live countdown `MM:SS` |
| DEFUSED | Hold final remaining time for up to 5 minutes or until reset |
| DETONATED | Hold final time/result for up to 5 minutes or until reset |

### 6.8 Optional Keep-Sync Process

When keep-sync is enabled in configuration:

- Device subscribes to controller game-state topic(s) and tracks remote `timeRemaining` and mode (`running`, `paused`, etc.).
- Local displayed time is corrected to remain within ±1 second of controller time.
- During WiFi/MQTT outages, the device continues local timing and resynchronizes after reconnect.
- Keep-sync is optional and disabled by default.

#### 6.8.1 Keep-Sync Topic Subscription and Schema

**Configuration (Web UI):**

- `game_state_topic`: Configurable topic to subscribe to authoritative game state (default: `paradox/game/state`)
- `prop_state_topic`: Configurable topic for prop/app online heartbeat feed (default: `paradox/state`)
- `time_tolerance_ms`: Max allowed time difference before correction (default: 1000 ms)

**Expected Game State Message Schema:**

```json
{
  "ts": 1712601234567,
  "gameMode": "running",
  "timeRemaining": 423,
  "gamePaused": false,
  "roundActive": true,
  "siteId": "location-1",
  "zoneId": "zone-a"
}
```

| Field | Type | Meaning |
|-------|------|---------|
| `ts` | number | Server millisecond timestamp |
| `gameMode` | string | `"running"`, `"paused"`, or `"idle"` |
| `timeRemaining` | number | Seconds remaining on game clock |
| `gamePaused` | boolean | True if game is paused (countdown halted) |
| `roundActive` | boolean | True if a puzzle round is actively running |
| `siteId` | string | Game site/location identifier (optional context) |
| `zoneId` | string | Zone identifier (optional context) |

**Sync Behavior:**

1. On message receipt, device compares remote `timeRemaining` and `gameMode` to local state.
2. If difference exceeds `time_tolerance_ms` OR mode differs: adjust local timer, update state flags, publish `syncAdjusted` event.
3. During outages: continue local countdown independently, buffer events with timestamps.
4. On reconnect: resubscribe, retrieve latest game state, resync.
5. On deep-sleep wake: reconnect, retrieve game state, publish `reconnected` event, ready for `start` command.
6. When keep-sync is enabled, inbound `start/pause/resume/setTime` commands are accepted but authoritative game-state topic values override local command effects.

**Conflict window / de-dupe rule:**

- Apply a command dedupe window of 750 ms (configurable) keyed by `{command, source, ts}`.
- If a keep-sync state update arrives inside the same 750 ms window and conflicts with command result, keep-sync state wins.
- Publish `commandOverridden` event when this occurs.

**When Disabled:**
- Device does not subscribe to game state topics.
- Responds only to explicit MQTT commands (start, pause, resume, etc.).
- Heartbeat publishing (if enabled) continues independently.

#### 6.8.2 Prop State and Announce Strategy

The device implements the Paradox standard prop announce + state pattern:

| Channel | Cadence | Default topic | Purpose |
|---------|---------|---------------|---------|
| **Announce** | Once per MQTT connect/reconnect | `paradox/props` | Discovery for PxH props panel / PxP catalog. Third-party installs may use `<company>/props`. |
| **State (heartbeat)** | Connect, on change, every ~10s | `{mqttBaseTopic}/state` (e.g. `paradox/<room>/<device>/state`) | Retained live prop snapshot |

Do **not** publish periodic heartbeats on the announce topic.

State/Announce JSON (illustrative; see `docs/api.md` for the live schemas):

```json
{
  "ts": 1712601234567,
  "propId": "wire-defusal-1",
  "status": "online",
  "gameState": "countdown",
  "uptime": 43200,
  "ip": "192.168.4.15",
  "rssi": -62,
  "battery": 87,
  "reconnectReason": "power-loss",
  "buildId": "28fda08",
  "buildDate": "2026-04-08",
  "buildTime": "13:21:07"
}
```

| Field | Type | Optional | Meaning |
|-------|------|----------|---------|
| `ts` | number | No | Millisecond timestamp |
| `propId` | string | No | Device identifier |
| `status` | string | No | `"online"`, `"offline"`, `"error"` |
| `gameState` | string | No | Current state (ready, countdown, detonated, defused) |
| `uptime` | number | No | Seconds since boot |
| `ip` | string | No | Current IP address |
| `rssi` | number | No | WiFi signal strength (dBm, negative) |
| `battery` | number | No | Battery capacity estimate (0–100%) |
| `reconnectReason` | string | Yes | Only in announce: e.g., `"power-loss"`, `"wifi-drop"`, `"mqtt-timeout"` |
| `buildId` | string | Yes | Build identifier (version hash/tag) when available |
| `buildDate` | string | Yes | Build date when available |
| `buildTime` | string | Yes | Build time when available |

---

## 7. MQTT Interface

All communication follows the **Paradox v2 MQTT Protocol** (see `PR_PX_APP_COMM_UPDATE.md`).

**Base topic:** `paradox/{site}/{zone}/`  
**Default zone:** `wire-defusal` (configurable)

### 7.1 Inbound Commands (subscribe to `.../commands`)

| Command | Payload | Description |
|---------|---------|-------------|
| `start` | `{"command": "start"}` | Begin/resume countdown immediately from current timer value. |
| `start` | `{"command": "start", "time": 900}` | Set time and begin countdown immediately in one command. |
| `stop` | `{"command": "stop"}` | Halt countdown immediately. Remain in current state. |
| `pause` | `{"command": "pause"}` | Pause countdown. Timer holds. Display blinks. |
| `resume` | `{"command": "resume"}` | Alias of `start`; begins countdown immediately from current timer value. |
| `reset` | `{"command": "reset"}` | Return to READY/NOT_READY based on current wire connectivity. Clear round state. |
| `setTime` | `{"command": "setTime", "time": 180}` | Update countdown to specific value (seconds). |
| `setSequence` | `{"command": "setSequence", "solution": "3124", "wireCount": 4}` | Set required disconnect order by input index vector string. Sequence length is derived from `solution`. |
| `setMode` | `{"command": "setMode", "mode": "penalty", "maxTries": 3}` | Configure wrong-wire mode and retry limit for active round. |
| `setPenalty` | `{"command": "setPenalty", "amount": 30}` | Configure time penalty seconds for active round. |
| `setLidMode` | `{"command": "setLidMode", "mode": "ignore|normallyOpen|normallyClosed"}` | Set lid behavior for active round. |
| `solve` | `{"command": "solve"}` | Force transition to `DEFUSED`. Only accepted during active countdown/paused states. |
| `fail` | `{"command": "fail", "reason": "controller"}` | Force transition to `DETONATED`. Only accepted during active countdown/paused states. |
| `wake` | `{"command": "wake"}` | Force wake/reconnect cycle and immediate state publish (for power-save scheduling). |
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
  "readyState": "ready",
  "timeRemaining": 142,
  "wiresDisconnected": [3, 1],
  "wiresRemaining": [4, 2],
  "correctSoFar": true,
  "mode": "penalty",
  "maxTries": 3,
  "triesUsed": 1,
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
| `detonated` | `{"event": "detonated", "ts": ..., "data": {"reason": "wrongWire", "wire": 2}}` | Wrong wire pulled (`instant` mode) |
| `penaltyApplied` | `{"event": "penaltyApplied", "ts": ..., "data": {"wire": 2, "penalty": 30, "newTime": 112}}` | Time deducted for wrong wire |
| `stateChanged` | `{"event": "stateChanged", "ts": ..., "data": {"from": "not_ready", "to": "ready"}}` | Any game state transition |
| `timerSync` | `{"event": "timerSync", "ts": ..., "data": {"time": 142}}` | Periodic time broadcast (every 10s during countdown) |
| `syncAdjusted` | `{"event": "syncAdjusted", "ts": ..., "data": {"before": 420, "after": 419}}` | Keep-sync corrected local timer to match controller |
| `commandOverridden` | `{"event": "commandOverridden", "ts": ..., "data": {"command": "resume", "reason": "keepSyncStateAuthoritative"}}` | Local command accepted but overridden by keep-sync state update |

### 7.4 Outbound Warnings (publish to `.../warnings`)

| Warning | When |
|---------|------|
| `{"warning": "lowBattery", "ts": ..., "message": "Battery voltage below 6.5V", "data": {"voltage": 6.3}}` | Battery getting low |
| `{"warning": "wifiReconnect", "ts": ..., "message": "WiFi connection lost, reconnecting"}` | WiFi dropped |
| `{"warning": "mqttReconnect", "ts": ..., "message": "MQTT broker connection lost"}` | MQTT dropped |

If WiFi or MQTT drops mid-game, the prop continues locally. Events are buffered with timestamps and replayed in order when connectivity returns.

---

## 8. Web Interface

### 8.1 Configuration Page (`http://<device-ip>/config.html`)

| Section | Fields |
|---------|--------|
| **Network** | WiFi SSID, Password, MQTT broker address, MQTT port |
| **Identity** | Site name, Zone name, Device ID |
| **Puzzle** | Default countdown time, Wire count, Wire sequence, Wrong-wire mode, Penalty amount, Max tries, Lid mode |
| **Inputs** | Input labels (`INPUT_1..INPUT_8` names), active input count, solution vector string |
| **Audio** | Success sound option, Success melody string, Failure sound option, Failure melody string |
| **Input Filtering** | (Developer only in config file) Debounce check interval (ms), consecutive readings threshold |
| **Display/LED** | (Developer only in config file) LED brightness (default 20%) |
| **Battery** | Battery profile, Low-battery threshold (%) |
| **Power Save** | Power-save mode (`none`, `displayBlank`, `displayRailGated`, `lightSleep`, `deepSleep`), pre-start wake window |
| **Sync** | Keep-sync enable, controller state topic |
| **Telemetry** | Heartbeat publish interval (default 10s) |
| **System** | Firmware version, Uptime, Free heap, OTA update URL + trigger |

At the bottom of the Configuration page, include a **Raw JSON Command** pane (POST `/api/command`) for developer diagnostics.

There is no separate Commands page in the v1 Web UI.

`Save` on this page updates persistent defaults stored in a local configuration file loaded at boot.

MQTT (or other comms) parameter updates are temporary for the current runtime/session unless explicitly saved via the configuration UI/API.

Connection page includes explicit MQTT topic fields at the bottom of the MQTT panel:

- `commands`
- `state`
- `events`
- `warnings`
- `game_state_topic`
- `prop_state_topic`

Connection page layout order:

1. WiFi Connection
2. Device Details
3. MQTT Connection
4. MQTT Topic Settings

WiFi scan behavior on Connection page:

- SSID scan runs automatically every 10 seconds.
- Manual `Scan SSIDs` remains available.
- Signal strength is shown as bar icon + RSSI dBm.

Icon style:

- WiFi and battery indicators use a matching line-icon visual style for consistency.
- Battery icon uses graphical fill level + color to indicate charge/warning status.

Device Details panel fields:

- Read-only: Prop Name (id), IP Address, Software Version, Build Number, Build Date, CPU Temp (if available), Free Memory, current battery percentage.
- Editable: Network Name (mDNS host label) with `.local` suffix shown in UI.
- Apply action updates mDNS hostname at runtime.

mDNS hostname behavior:

- Default network name is derived from prop id with a unique suffix (e.g. `px-wifi-v1-a1b2`), so URL resolves as `http://px-wifi-v1-a1b2.local`.
- User may override network name from Connection page.

### 8.2 Live Status (`http://<device-ip>/index.html`, `ws://<device-ip>/ws`)

WebSocket pushes the same JSON events as MQTT in real time. Useful for a technician standing next to the prop with a phone — no MQTT broker needed for local debugging.

In the Live panel, **Tries Used** is shown only when mode is `buzz` or `penalty`. It is hidden for `instant` mode.

---

## 9. LED Status Indicator

| Color | Pattern | Meaning |
|-------|---------|---------|
| Magenta | Solid | AP mode — waiting for WiFi configuration |
| Blue | Slow pulse (1 Hz) | Connecting to WiFi |
| Cyan | Double blink | Connected to WiFi, connecting to MQTT |
| Green | Solid | Online and READY |
| Yellow | Slow pulse (1 Hz) | Online but NOT_READY |
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
| Countdown time | 3600 seconds (60:00) | MQTT, Web UI |
| Input labels | `INPUT_1=red`, `INPUT_2=green`, `INPUT_3=yellow`, `INPUT_4=blue`, `INPUT_5=white`, `INPUT_6=orange`, `INPUT_7=brown`, `INPUT_8=purple` | Web UI |
| Solution vector | `"1234"` | MQTT, Web UI |
| Required sequence length | Derived from `solution` length | Derived |
| Wrong-wire mode | `penalty` | MQTT, Web UI |
| Penalty amount | 30 seconds | MQTT, Web UI |
| Max tries | 3 | MQTT, Web UI |
| Lid mode | `ignore` | MQTT, Web UI |
| Debounce check interval | 10 ms | Config file only |
| Debounce consecutive reads | 5 | Config file only |
| Heartbeat interval | 10 seconds | Web UI |
| Heartbeat topic | `paradox/state` | Web UI |
| Low Battery Notification % | 40% | Web UI |
| Battery Cutoff % | 20% | Web UI |
| LED brightness | 20% | Config file only |
| WiFi AP timeout | 30 seconds | Config file only |
| Success sound | `beeps` | Web UI |
| Failure sound | `buzz` | Web UI |
| Success melody | `C6-8,D6-8,E6-4` | Web UI |
| Failure melody | `E5-4,D5-4,C5-4,B4-4` | Web UI |
| Power-save mode | `lightSleep` | Active (not configurable in v1.0) |
| Keep-sync enabled | Disabled | Web UI |
| Game state topic | `paradox/game/state` | Web UI |
| Command topic | `paradox/game/zone/commands` | Web UI |
| State topic | `paradox/game/zone/state` | Web UI |
| Events topic | `paradox/game/zone/events` | Web UI |
| Warnings topic | `paradox/game/zone/warnings` | Web UI |
| Time tolerance (keep-sync) | 1000 ms | Config file only |
| Keep-sync max drift | 1 second (1000 ms) | Config file only |
| Command dedupe window | 750 ms | Config file only |
| Battery profile default | `unknown` | Web UI |
| mDNS network name | Derived from prop id + unique suffix | Web UI |

## 10. Configuration & Tuning

### 10.1 Battery Monitoring Settings

| Setting | Default | Web UI | Description |
|---------|---------|--------|-------------|
| Low Battery Notification % | 40% | Yes | Battery capacity threshold (0–100%) that triggers low-battery alerts. When battery drops below this level, a triple-beep alert plays every 5 minutes and an MQTT warning is published to the warnings topic. |
| Battery Cutoff % | 20% | Yes | Battery capacity threshold (0–100%) that triggers deep-sleep shutdown. If battery stays at or below this level for 15+ seconds, the device enters low-power sleep mode. Valid range: 20% to <Low Battery Notification %. |

**Behavior:**

- **Low Battery Notification:** Device remains online and operational but emits repeated alerts (very low-pitched triple beep, repeating every 5 minutes).
- **Battery Cutoff:** Device powers down non-essential subsystems (WiFi, display, buzzer) and enters deep-sleep mode to preserve battery. GPIO inputs remain active to detect wake events. Plays a shutdown beep (~1.4 seconds) before entering sleep.
- **Deep Sleep Wake:** On wake (via Reset button or power reconnection), device reconnects to WiFi and MQTT, publishes a `reconnected` event, and resumes normal operation.

### 10.2 Battery Profiles

Battery profiles are defined in the persistent configuration file as voltage-to-capacity lookup arrays.

Required initial profiles:

- `6v-lead-acid`
- `6v-LiFePO4`
- `12v-lead-acid`
- `12v-LiFePO4`
- `external`
- `unknown`

Each profile maps measured battery voltage to approximate remaining capacity (%). Low-battery notifications trigger when computed capacity falls below `lowBatteryPercent` (default 40); deep-sleep shutdown triggers when capacity falls below `lowBatteryCutoffPercent` (default 20).

Battery profile storage also includes voltage-divider calibration and raw ADC correlation:

- `adcAt0V`: ADC raw reading corresponding to 0V input
- `adcAt15V`: ADC raw reading corresponding to 15V input
- `adcRaw`: current/raw sampled ADC value

Runtime voltage is calculated by linear interpolation between those calibration points, then used for battery capacity interpolation.

For `external` and `unknown` profiles:

- Battery displays as `100%`
- Status remains green unless interpolated voltage drops below 5.0V
- If voltage drops below 5.0V, status switches to warning (orange)

---

## 11. Open Questions and Clarifications

### Resolved from Latest Review

- No tenths-of-a-second display; keep 4-digit `MM:SS` with blinking colon behavior.
- No hint mode in v1.
- On connectivity loss, continue locally and replay buffered timestamped events after reconnect.
- Keep v1 single-purpose (wire defusal), but leave architecture open for future I/O expansion.
- Target battery runtime is 8+ hours; power-saving strategy will be refined separately.
- Lid switch is optional and defaults to `ignore`.
- Wire reconnection behavior applies only in `buzz` and `penalty` modes.
- Support up to 8 wires in architecture, with sequence length configurable from 1 to wire count.
- Reconnection backoff: exponential 1s → 2s → 4s → 8s → 16s → 32s (capped), resets to 1s after successful reconnect.
- Solve/fail commands only accepted during active countdown/paused states.
- Keep-sync watches game state and syncs local time/mode automatically within ±1s drift.
- MML-format melodies for success/failure (examples provided in Section 6.6.1).

### Persistence Model Question Clarification

**Persistence model** refers to how configuration is saved and loaded across device reboots:

1. **NVS-only:** Use ESP32's encrypted key-value storage. Fast, secure, but opaque (not human-readable).
2. **JSON-file-only:** Store config in a JSON file on SPIFFS/FAT. Human-readable, editable offline, slower.
3. **Hybrid (NVS + JSON mirror):** Keep config in both places for speed + inspectability. More complex.

**Chosen approach:** JSON configuration file in SPIFFS. The web UI saves changes to the JSON file. On boot, the device loads config from the file into runtime structures.
