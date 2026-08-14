# px-wifi-v1 — API Reference

All interfaces are available once the device is on your network (STA mode) or via its own access point (AP mode). The device exposes four integration surfaces: an **HTTP REST API**, a **WebSocket**, an **MQTT interface**, and an **OTA update endpoint**.

---

## 1. Addressing

| Mode | Address |
|------|---------|
| STA IP | shown on web UI / `GET /api/device/details` → `ipAddress` |
| AP IP | `192.168.4.1` (default) |
| AP SSID | `Paradox-PXWiFiV1-XXYY` (last 2 bytes of AP MAC) |
| mDNS | `<networkName>.local` (default `px-wifi-v1-XXYY.local`) |

Replace `<host>` in every example below with the device IP or mDNS name.

---

## 2. HTTP REST API

All endpoints run on **port 80**. Requests and responses use `application/json` unless otherwise stated. There is no authentication on any HTTP endpoint.

### 2.1 Static assets

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/` | Web UI (index.html) |
| `GET` | `/index.html` | Web UI (index.html) |
| `GET` | `/config.html` | Configuration page |
| `GET` | `/connection.html` | Connection settings page |
| `GET` | `/update` | OTA update page |
| `GET` | `/styles.css` | Stylesheet |
| `GET` | `/app.js` | Frontend script |
| `GET` | `/assets/logo.png` | Logo (cached 1 h) |

### 2.2 State

#### `GET /api/state`

Returns a live prop snapshot (same schema as the MQTT `state` topic).

```bash
curl http://<host>/api/state
```

Example response:
```json
{
  "ts": 84231,
  "id": "px-wifi-v1",
  "status": "online",
  "gameState": "countdown",
  "timeRemaining": 142,
  "triesUsed": 1,
  "maxTries": 3,
  "mode": "penalty",
  "solution": "1234",
  "disconnectedOrder": "1",
  "wireCount": 4,
  "connectedMask": 14,
  "battery": 87,
  "batteryVoltageMv": 6500,
  "lowBattery": false
}
```

| Field | Type | Description |
|-------|------|-------------|
| `ts` | number | Milliseconds since boot |
| `id` | string | Always `"px-wifi-v1"` |
| `status` | string | Always `"online"` |
| `gameState` | string | `"not_ready"` \| `"ready"` \| `"countdown"` \| `"paused"` \| `"defused"` \| `"detonated"` |
| `timeRemaining` | number | Seconds remaining on the timer |
| `triesUsed` | number | Wrong-wire attempts this round |
| `maxTries` | number | Attempt limit (from config) |
| `mode` | string | `"instant"` \| `"penalty"` \| `"buzz"` |
| `solution` | string | Required wire-disconnect sequence, e.g. `"1234"` |
| `disconnectedOrder` | string | Wire indices disconnected so far, in order |
| `wireCount` | number | Number of active inputs (0–8) |
| `connectedMask` | number | Bitmask — bit 0 = input 1, bit 1 = input 2, … |
| `battery` | number | Battery percent (0–100) |
| `batteryVoltageMv` | number | Battery voltage in mV |
| `lowBattery` | boolean | True when below `lowBatteryPercent` threshold |

### 2.3 Commands

#### `POST /api/command`

Executes a prop-engine command. See section 4 for all commands and their parameters. Publishes state and events to MQTT after execution.

```bash
curl -X POST http://<host>/api/command \
  -H 'Content-Type: application/json' \
  -d '{"command":"start"}'
```

### 2.4 Config

#### `GET /api/config`

Returns the full unified runtime configuration (prop settings + connection settings + computed MQTT topics).

```bash
curl http://<host>/api/config
```

Example response (abbreviated):
```json
{
  "wifiSsid": "MyNetwork",
  "wifiPassword": "secret",
  "mqttHost": "192.168.1.50",
  "mqttPort": 1883,
  "mqttBaseTopic": "paradox/room/device",
  "mqttCommandTopic": "paradox/room/device/commands",
  "mqttStateTopic": "paradox/room/device/state",
  "mqttEventsTopic": "paradox/room/device/events",
  "mqttWarningsTopic": "paradox/room/device/warnings",
  "mqttGameStateTopic": "paradox/room/state",
  "mqttPropAnnounceTopic": "paradox/props",
  "networkName": "px-wifi-v1-a1b2",
  "apEnabled": true,
  "defaultTime": 3600,
  "penalty": 30,
  "maxTries": 3,
  "wireCount": 4,
  "mode": "penalty",
  "solution": "1234",
  "batteryProfile": "unknown",
  "heartbeatInterval": 10000
}
```

#### `GET /api/config/defaults`

Returns factory-default values for all prop-engine config fields. Same schema as `GET /api/config`.

```bash
curl http://<host>/api/config/defaults
```

#### `POST /api/config`

Applies a partial or full config update to RAM (not persisted). Only the fields present in the request body are changed.

```bash
curl -X POST http://<host>/api/config \
  -H 'Content-Type: application/json' \
  -d '{"maxTries":5,"penalty":60}'
```

Response: `{ "ok": true, "persisted": false, "spiffs": true }`

#### `POST /api/config/save`

Same as `POST /api/config` but also persists to SPIFFS (`/spiffs/config.json`).

```bash
curl -X POST http://<host>/api/config/save \
  -H 'Content-Type: application/json' \
  -d '{"defaultTime":900,"solution":"2413"}'
```

Response: `{ "ok": true, "persisted": true, "spiffs": true }`

#### `POST /api/config/restore`

Restores factory defaults in RAM (not persisted).

```bash
curl -X POST http://<host>/api/config/restore -d '{}'
```

Response: `{ "ok": true, "persisted": false, "spiffs": true }`

#### `POST /api/config/restore/save`

Restores factory defaults and persists to SPIFFS.

```bash
curl -X POST http://<host>/api/config/restore/save -d '{}'
```

Response: `{ "ok": true, "persisted": true, "spiffs": true }`

### 2.5 Connection

#### `GET /api/connection`

Returns connection settings and computed MQTT topics.

```bash
curl http://<host>/api/connection
```

Example response:
```json
{
  "wifiSsid": "MyNetwork",
  "wifiPassword": "secret",
  "mqttHost": "192.168.1.50",
  "mqttPort": 1883,
  "mqttUsername": "",
  "mqttPassword": "",
  "mqttBaseTopic": "paradox/room/device",
  "mqttCommandTopic": "paradox/room/device/commands",
  "mqttStateTopic": "paradox/room/device/state",
  "mqttEventsTopic": "paradox/room/device/events",
  "mqttWarningsTopic": "paradox/room/device/warnings",
  "mqttGameStateTopic": "paradox/room/state",
  "mqttPropAnnounceTopic": "paradox/props",
  "networkName": "px-wifi-v1-a1b2",
  "apSsid": "Paradox-PXWiFiV1-A1B2",
  "apPassword": "",
  "apIpAddress": "192.168.4.1",
  "apEnabled": true
}
```

#### `POST /api/connection`

Update WiFi and/or MQTT connection settings. Always persists to SPIFFS. Triggers WiFi reconnect if credentials changed; updates mDNS hostname if `networkName` changed.

```bash
curl -X POST http://<host>/api/connection \
  -H 'Content-Type: application/json' \
  -d '{"mqttHost":"10.0.0.5","mqttBaseTopic":"site/hall/bomb"}'
```

Responses:

| Scenario | Response |
|----------|----------|
| WiFi fields changed, connected | `{"ok":true,"applied":true,"connecting":true}` |
| WiFi fields changed, failed | `{"ok":false,"applied":true,"connecting":false,"error":"ESP_ERR_..."}` |
| No WiFi fields | `{"ok":true,"applied":true,"connecting":false}` |
| Invalid credentials (HTTP 400) | `{"ok":false,"applied":false,"error":"SSID 'X' requires a WiFi password."}` |

WiFi password validation:
- Passwords > 64 chars → rejected
- Protected network with no password → rejected
- Open/OWE network with a password → rejected
- Enterprise auth → rejected (not supported)
- Password < 8 chars on WPA → rejected

#### `GET /api/connection/scan`

Triggers a blocking WiFi scan (up to 16 APs) and returns results.

```bash
curl http://<host>/api/connection/scan
```

Example response:
```json
{
  "ok": true,
  "networks": [
    { "ssid": "MyNetwork", "rssi": -55, "auth": 3, "authName": "wpa2-psk" },
    { "ssid": "OpenNet",   "rssi": -70, "auth": 0, "authName": "open" }
  ]
}
```

### 2.6 Device details

#### `GET /api/device/details`

Returns device identity, firmware version, and live connectivity status.

```bash
curl http://<host>/api/device/details
```

Example response:
```json
{
  "propName": "px-wifi-v1-a1b2",
  "ipAddress": "192.168.1.55",
  "softwareVersion": "0.1.0",
  "buildNumber": "0.1.0",
  "buildDate": "May 20 2026 10:00:00",
  "cpuTempC": 48.2,
  "freeMemoryBytes": 243712,
  "batteryPercent": 87,
  "batteryState": "normal",
  "batteryVoltageMv": 7850,
  "batteryAdcRaw": 1680,
  "lowBattery": false,
  "networkName": "px-wifi-v1-a1b2",
  "status": "ready",
  "apIpAddress": "192.168.4.1",
  "wifiConnected": true,
  "wifiConnecting": false,
  "wifiTargetSsid": "MyNetwork",
  "wifiSsid": "MyNetwork",
  "wifiRssi": -55,
  "wifiLastError": "",
  "wifiLastErrorCode": 0,
  "pendingApShutdown": false
}
```

`batteryVoltageMv` is the converted sense-pin voltage and is present even when
`batteryState` is `"usb"`. `batteryAdcRaw` is the oversampled raw ADC count used
for that conversion.

#### `POST /api/device/name`

Updates the mDNS hostname at runtime (not persisted to SPIFFS). Name is lowercased; only `a-z`, `0-9`, and `-` are kept; maximum 32 characters.

```bash
curl -X POST http://<host>/api/device/name \
  -H 'Content-Type: application/json' \
  -d '{"networkName":"bomb-room-1"}'
```

Response: `{ "ok": true, "networkName": "bomb-room-1", "url": "http://bomb-room-1.local" }`

### 2.7 OTA firmware update

#### `POST /api/ota/upload`

Streams a raw firmware binary into the next OTA partition. The device reboots automatically ~1.2 s after a successful write.

- **Body:** raw `.bin` binary (not multipart) — `Content-Length` header required
- **Authentication:** none
- **Build artefact:** `.pio/build/...` or `build/...` depending on build system

```bash
curl -X POST http://<host>/api/ota/upload \
  --data-binary @build/px-wifi-v1.bin
```

Responses:

| Outcome | Body |
|---------|------|
| Success | `{"ok":true,"message":"OTA complete, rebooting"}` |
| No payload | HTTP 500 `"Missing firmware payload"` |
| No OTA partition | HTTP 500 `"No OTA partition"` |
| Write failure | HTTP 500 `"OTA write failed"` |
| Finalise failure | HTTP 500 `"OTA finalize failed"` |
| Boot-partition failure | HTTP 500 `"OTA boot partition failed"` |

---

## 3. WebSocket

#### `GET /ws` (upgraded)

Accepts WebSocket text frames containing the same command JSON as `POST /api/command`. Returns the command response as a text frame.

```
ws://<host>/ws
```

Empty frames → `{"ok":false,"error":"emptyPayload"}`

---

## 4. Commands

Commands are accepted via:
- `POST /api/command` (HTTP)
- WebSocket text frames on `/ws`
- MQTT `{base}/commands` topic

All commands use `{"command": "<name>", ...parameters}`. Commands are deduplicated: if the same JSON body is received again within `dedupeWindowMs` (default 750 ms), the second call returns `{"ok":true,"deduped":true}` with no state change.

### `ping`

Health check.

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"ping"}'
```

Response: `{"event":"pong","ts":<ms>}`

---

### `getState`

Forces an immediate state publish and returns the full state object.

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"getState"}'
```

Response: full state JSON (same as `GET /api/state`).

---

### `start`

Start the countdown. Device must be in `ready` state (all wires connected).

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `time` | int \| string | No | Override timer. Integer = seconds; `"MM:SS"` or `"HH:MM:SS"` string also accepted |

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"start"}'
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"start","time":900}'
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"start","time":"15:00"}'
```

Responses:

| Outcome | Response |
|---------|----------|
| Success | `{"ok":true,"state":"countdown"}` |
| Wires not connected | `{"ok":false,"error":"notReady","state":"not_ready","disconnected":"red, green","details":"Inputs not closed: red, green"}` |

---

### `resume`

Resume from `paused`, `ready`, or `not_ready` → `countdown`.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `time` | int \| string | No | Override timer (same formats as `start`) |

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"resume"}'
```

Response: `{"ok":true,"state":"countdown"}` or `{"ok":false,"error":"notPaused"}`

---

### `pause`

Pause a running countdown.

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"pause"}'
```

Response: `{"ok":true,"state":"paused"}` or `{"ok":false,"error":"notRunning"}`

---

### `stop`

Same as `pause` but also sets a `stopped` flag that causes the display to blink at 1 Hz.

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"stop"}'
```

Response: `{"ok":true,"state":"paused"}` or `{"ok":false,"error":"notRunning"}`

---

### `reset`

Reset the round. Restores `defaultTime`, clears tries and disconnect order, returns to `ready` or `not_ready`.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `time` | int \| string | No | Set a specific time instead of restoring `defaultTime` |

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"reset"}'
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"reset","time":600}'
```

Response: `{"ok":true,"state":"ready"}` or `{"ok":true,"state":"not_ready","disconnected":"..."}`

---

### `setTime`

Update the timer value without changing game state.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `time` | int \| string | Yes | Seconds (int) or `"MM:SS"` / `"HH:MM:SS"` |

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"setTime","time":300}'
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"setTime","time":"5:00"}'
```

Response: `{"ok":true,"time":300}` or `{"ok":false,"error":"missingTime"}`

---

### `setMode`

Change the puzzle mode.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `mode` | string | Yes | `"instant"` \| `"penalty"` \| `"buzz"` |
| `maxTries` | integer | No | 1–100; only used in `penalty` / `buzz` modes |

| Mode | Behaviour |
|------|-----------|
| `instant` | First wrong wire detonates immediately |
| `penalty` | Wrong wire subtracts `penalty` seconds; `maxTries` attempts before detonation |
| `buzz` | Wrong wire triggers buzzer but no penalty; `maxTries` attempts before detonation |

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands \
  -m '{"command":"setMode","mode":"penalty","maxTries":3}'
```

Response: `{"ok":true,"mode":"penalty"}` or `{"ok":false,"error":"invalidMode"}`

---

### `setPenalty`

Set the per-wrong-wire time penalty (used in `penalty` mode).

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `amount` | integer | Yes | Seconds (0–3600) |

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands \
  -m '{"command":"setPenalty","amount":60}'
```

Response: `{"ok":true,"penalty":60}` or `{"ok":false,"error":"invalidPenalty"}`

---

### `setSequence`

Set the wire-disconnect solution and wire count. Resets the current round.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `solution` | string | Yes | Digit string `"1"`–`"8"`, max 8 chars, each digit ≤ `wireCount` |
| `wireCount` | integer | No | 1–8 (defaults to current) |

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands \
  -m '{"command":"setSequence","solution":"3124","wireCount":4}'
```

Response: `{"ok":true,"solution":"3124"}` or `{"ok":false,"error":"invalidSolution"}`

---

### `setLidMode`

Configure how the lid sensor input is interpreted.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `mode` | string | Yes | `"off"` \| `"closed"` \| `"open"` \| `"ignore"` \| `"normallyClosed"` \| `"normallyOpen"` |

`"ignore"` maps to `"off"`; `"normallyClosed"` maps to `"closed"`; `"normallyOpen"` maps to `"open"`.

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands \
  -m '{"command":"setLidMode","mode":"closed"}'
```

Response: `{"ok":true,"lidMode":"closed"}` or `{"ok":false,"error":"invalidLidMode"}`

---

### `solve`

Force the prop into `defused` state. Only valid during `countdown` or `paused`.

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"solve"}'
```

Response: `{"ok":true,"state":"defused"}` or `{"ok":false,"error":"notActive"}`

---

### `fail`

Force the prop into `detonated` state. Sets `timeRemaining` to 0. Only valid during `countdown` or `paused`.

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"fail"}'
```

Response: `{"ok":true,"state":"detonated"}` or `{"ok":false,"error":"notActive"}`

---

### `disconnect` (simulation)

Simulate a wire disconnect on the given input. Useful for testing without hardware.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `input` | integer | Yes | Input number 1–8 |

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands \
  -m '{"command":"disconnect","input":2}'
```

Response: `{"ok":true,"input":2}` or `{"ok":false,"error":"missingInput"}`

---

### `connect` (simulation)

Simulate a wire reconnect on the given input.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `input` | integer | Yes | Input number 1–8 |

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands \
  -m '{"command":"connect","input":2}'
```

Response: `{"ok":true,"input":2}` or `{"ok":false,"error":"missingInput"}`

---

### `wake` / `identify`

Acknowledge presence (no side-effects beyond acknowledgement).

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"identify"}'
```

Response: `{"ok":true,"command":"wake"}`

---

### `reboot`

Schedule a device reboot (~1.2 s delay).

```bash
mosquitto_pub -h <broker> -t paradox/room/device/commands -m '{"command":"reboot"}'
```

Response: `{"ok":true,"command":"reboot"}`

---

### Unknown command

Response: `{"ok":false,"error":"unknownCommand"}`

---

## 5. MQTT

All topics are rooted at the configured `mqttBaseTopic` (default `paradox/room/device`). The device connects with a persistent session (clean session = false), client ID = `networkName`, keep-alive 60 s, QoS 1 for all publishes and subscriptions.

### 5.1 Topic map

| Topic | Direction | Retained | Description |
|-------|-----------|----------|-------------|
| `<base>/commands` | → device | No | Send commands (see section 4) |
| `<base>/state` | ← device | **Yes** | Periodic state heartbeat + current snapshot |
| `<base>/events` | ← device | No | Prop events and command outcomes |
| `<base>/warnings` | ← device | No | Rejected commands and errors |
| `<mqttPropAnnounceTopic>` | ← device | No | Online announcement on each connect |
| `<mqttGameStateTopic>` | → device | — | Game-master timer sync (when `keepSyncEnabled = true`) |

### 5.2 State topic (`<base>/state`)

Published on connect, after every state-changing command, and on the heartbeat interval (default 10 s, configurable 1–120 s). Retained.

See `GET /api/state` in section 2.2 for the full schema.

### 5.3 Events topic (`<base>/events`)

All event messages share this schema:

```json
{
  "event": "<event>",
  "ts": 84231,
  "state": "defused",
  "timeRemaining": 42,
  "triesUsed": 1,
  "maxTries": 3,
  "mode": "penalty"
}
```

Game events:

| `event` | Trigger |
|---------|---------|
| `defused` | Correct wire sequence completed |
| `detonated` | Timer expired, wrong wire in `instant` mode, or `maxTries` exceeded |
| `BAD-ATTEMPT` | Wrong wire in `buzz` or `penalty` mode (tries remaining) |

Command responses are also published to the events topic when a command is received via MQTT.

Keep-sync events (when `keepSyncEnabled = true`):

| `event` | `data` fields | Trigger |
|---------|--------------|---------|
| `syncAdjusted` | `before`, `after` (seconds), `message` | Local timer corrected to match game master |
| `commandOverridden` | `before`, `after` (seconds), `message` | Local pause/resume overridden to match game state |

### 5.4 Warnings topic (`<base>/warnings`)

```json
{ "ts": 84231, "warning": "command_rejected" }
```

Published when a command response has `ok: false` or contains an `error` field.

### 5.5 Announce topic (`<mqttPropAnnounceTopic>`)

Published **once** on each successful MQTT connect or reconnect (not on the heartbeat
interval). Default topic: `paradox/props`.

Third-party venues may set this to `<company>/props` when they want their own namespace;
the suite default remains `paradox/props`. Periodic prop state belongs on
`<base>/state` (typically `paradox/<room>/<device>/state`), never on the announce topic.

```json
{
  "ts": 84231,
  "event": "online",
  "propId": "px-wifi-v1-a1b2",
  "propName": "px-wifi-v1-a1b2",
  "ip": "192.168.1.55",
  "mdns": "px-wifi-v1-a1b2.local",
  "wireCount": 4,
  "batteryProfile": "unknown",
  "ledHint": "ready",
  "version": "0.1.0",
  "buildId": "0.1.0",
  "buildDate": "May 20 2026",
  "buildTime": "10:00:00",
  "stateTopic": "paradox/room/device/state",
  "commandsTopic": "paradox/room/device/commands"
}
```

`ledHint` values: `"ap"`, `"wifi_connecting"`, `"mqtt_connecting"`, `"ready"`, `"not_ready"`, `"countdown"`, `"paused"`, `"penalty"`, `"detonated"`, `"defused"`, `"ota"`, `"off"`.

### 5.6 Keep-sync follower (`<mqttGameStateTopic>`)

When `keepSyncEnabled = true` the device subscribes to this topic and adjusts its local timer to match. Inbound payload:

```json
{
  "timeRemaining": 423,
  "gamePaused": false,
  "gameMode": "running",
  "state": "countdown"
}
```

Also accepts `remaining_time` as a `"MM:SS"` string in place of `timeRemaining`. Sync only fires when the drift exceeds `timeToleranceMs` (default 1000 ms).

---

## 6. Config schema

All fields can be set via `POST /api/config/save` or `POST /api/connection`. The SPIFFS store is `/spiffs/config.json`.

### Puzzle / game

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `defaultTime` | integer | `3600` | Timer seconds (10–86400) |
| `penalty` | integer | `30` | Per-wrong-wire penalty seconds (0–3600), `penalty` mode only |
| `maxTries` | integer | `3` | Wrong-wire attempts before detonation (1–100) |
| `wireCount` | integer | `4` | Active inputs (0–8) |
| `mode` | string | `"instant"` | `"instant"` \| `"penalty"` \| `"buzz"` |
| `lidMode` | string | `"off"` | `"off"` \| `"closed"` \| `"open"` |
| `solution` | string | `"1234"` | Digit string, each digit 1–8, max 8 chars |
| `holdResultSeconds` | integer | `60` | Seconds to hold `defused`/`detonated` before auto-reset |
| `keepSyncEnabled` | boolean | `false` | Enable game-master timer sync |
| `timeToleranceMs` | integer | `1000` | Sync drift threshold in ms (0–10000) |

### Input names

| Field | Type | Default |
|-------|------|---------|
| `input1Name` | string | `"red"` |
| `input2Name` | string | `"green"` |
| `input3Name` | string | `"yellow"` |
| `input4Name` | string | `"blue"` |
| `input5Name` | string | `"white"` |
| `input6Name` | string | `"orange"` |
| `input7Name` | string | `"brown"` |
| `input8Name` | string | `"purple"` |

### Network / MQTT

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `wifiSsid` | string | `""` | Max 32 chars |
| `wifiPassword` | string | `""` | Max 64 chars |
| `networkName` | string | `"px-wifi-v1-XXYY"` | mDNS hostname; max 32 chars, alphanumeric + `-` |
| `apPassword` | string | `""` | AP password (WPA2 if set); max 64 chars |
| `uiPassword` | string | `""` | Optional web UI login password (HTTP Basic Auth). Empty = no login required. |
| `apEnabled` | boolean | `true` | Keep AP running after STA connects |
| `mqttHost` | string | `""` | Max 127 chars |
| `mqttPort` | integer | `1883` | |
| `mqttUsername` | string | `""` | Max 63 chars |
| `mqttPassword` | string | `""` | Max 63 chars |
| `mqttBaseTopic` | string | `"paradox/room/device"` | Max 95 chars |
| `mqttGameStateTopic` | string | `"paradox/room/state"` | Max 127 chars; keep-sync source. Used as-is — include `/state` in the value; nothing is appended. |
| `mqttPropAnnounceTopic` | string | `"paradox/props"` | Max 127 chars; **one-shot** announce on connect/reconnect. May be `<company>/props` for third-party installs. |
| `heartbeatInterval` | integer (ms) | `10000` | MQTT state publish interval (1000–120000 ms) |

### Battery

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `batteryProfile` | string | `"unknown"` | `"6v-lead-acid"` \| `"6v-LiFePO4"` \| `"12v-lead-acid"` \| `"12v-LiFePO4"` \| `"12v-Li-ion"` \| `"external"` \| `"unknown"` |
| `batteryPoints` | string | — | Custom curve, CSV `"v:pct,v:pct,..."` (used when profile = `"custom"`) |
| `lowBatteryPercent` | integer | `40` | Threshold for `lowBattery` flag (0–100) |
| `lowBatteryCutoffPercent` | integer | `20` | Threshold for cutoff behaviour (0–100) |
| `batteryAdcAt0V` | integer | `0` | ADC raw value at 0 V |
| `batteryAdcAt15V` | integer | `4095` | ADC raw value at 15 V |

### Buzzer (MML strings)

| Field | Default |
|-------|---------|
| `buzzerStartResumeMml` | `"T200 O6 L32 V80 C R C"` |
| `buzzerPauseResetMml` | `"T200 O5 L32 V80 C R C"` |
| `buzzerSolvedMml` | `"T184 O5 L16 V78 C E G R C6 R C6 E6 G6 L8 C7"` |
| `buzzerFailedMml` | `"T108 O5 L16 V72 G F E R B4 R L8 G4"` |

### Advanced / commissioning

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `debounceCheckIntervalMs` | integer | `10` | Per-input poll interval (1–200 ms) |
| `debounceConsecutiveReads` | integer | `5` | Consecutive identical reads to confirm a state change (1–20) |
| `dedupeWindowMs` | integer | `750` | Command deduplication window (100–5000 ms) |
| `ledBrightnessPercent` | integer | `20` | LED brightness (1–100) |
| `wifiApTimeoutSec` | integer | `30` | Seconds before AP shuts down after STA connects (5–300) |

---

## 7. mDNS

The device registers on mDNS as `<networkName>.local` with an `_http._tcp` service on port 80. The hostname is derived from the last two bytes of the STA MAC by default (`px-wifi-v1-XXYY`). It can be changed at runtime via `POST /api/device/name` or persisted via `POST /api/connection`.
