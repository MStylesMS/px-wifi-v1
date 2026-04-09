# PX-WiFi-V1 Morning Test Checklist

This checklist covers the three deferred validation steps:

1. HTTP endpoint smoke test
2. Command flow validation
3. Config persistence save and reboot verification

## Preconditions

- Device is flashed with the latest `px-wifi-v1` firmware.
- Mac remains online through a second network path before joining the prop AP.
- Mac is connected to the prop SoftAP `Paradox-PXWiFiV1-9A51`.
- Device API base URL is `http://192.168.4.1`.

## Smoke Test

Run:

```sh
python3 scripts/prop_http_smoke_test.py --host 192.168.4.1 --mode smoke
```

Expected result:

- `/api/state` returns JSON with `gameState` and `timeRemaining`
- `/api/config` returns JSON with `defaultTime` and `wireCount`
- `/api/command` with `ping` returns `{"event":"pong"...}`
- `/api/command` with `getState` returns the same shape as `/api/state`

## Command Flow

Run:

```sh
python3 scripts/prop_http_smoke_test.py --host 192.168.4.1 --mode commands
```

This sends:

- `reset`
- `start`
- `pause`
- `resume`
- `disconnect input 1`
- `connect input 1`
- `solve`
- `reset`
- `start` with `time=120`
- `fail`
- `reset`

Check for:

- All responses are HTTP 200 with valid JSON
- No crashes or watchdog resets in serial log
- Final state returns to `ready` or `not_ready`

## Persistence Check

Phase 1, save config:

```sh
python3 scripts/prop_http_smoke_test.py --host 192.168.4.1 --mode persist-save
```

Expected saved values:

- `defaultTime = 222`
- `penalty = 17`
- `maxTries = 4`

Phase 2, reboot device manually:

- Press reset on the dev board, or power-cycle the device.

Note:

- The current `restart` command acknowledges success but does not yet trigger a real reboot, so use a manual reset for this check.

Phase 3, verify persisted values:

```sh
python3 scripts/prop_http_smoke_test.py --host 192.168.4.1 --mode persist-check
```

Expected result:

- `/api/config` still reports `defaultTime=222`, `penalty=17`, and `maxTries=4`

## If Something Fails

- Capture serial monitor logs during the failing request.
- Re-run only the failing phase instead of the full sequence.
- If persistence fails, inspect `/spiffs/config.json` behavior and SPIFFS mount logs first.