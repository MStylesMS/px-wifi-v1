# px-wifi-v1 — AI Instructions

ESP32 Wi-Fi smart-prop firmware for Paradox escape rooms.

Target **ESP-IDF 6.0.x** (same pin as `../px-components`). Admin UI chrome:
[docs/console-chrome.md](docs/console-chrome.md) — **source of truth for Signal Glass
look and responsive breakpoints** (phone / tablet / desktop) for all future
`px-*-v1` props. Local preview without flash:
`scripts/serve_webui.ps1` → `http://127.0.0.1:8090/index.html`.

SoftAP (`Paradox-PXWiFiV1-XXXX`): starts for recovery; **off by default after STA
connects** (`apEnabled=false`, same as fuse/valve/patch/dynamite). Check Connect
“Keep AP active while connected to WiFi” to leave it on.

## MQTT topic contract

| Topic | When | Default | Notes |
|-------|------|---------|-------|
| `{baseTopic}/state` | Connect, on change, every heartbeat (~10s) | `paradox/room/device/state` | Retained prop state / heartbeat. Prefer `paradox/<room>/<device>/state` in venues. |
| `{baseTopic}/commands` | Inbound | `…/commands` | Game / operator commands |
| `{baseTopic}/events` | Outbound | `…/events` | Events, not heartbeat |
| `{baseTopic}/warnings` | Outbound | `…/warnings` | Plural |
| Announce (`mqttPropAnnounceTopic`) | **Once** per MQTT connect/reconnect | `paradox/props` | Discovery bus for PxH / PxP catalog. Third-party installs may use `<company>/props`; do not put periodic heartbeats here. |

Do **not** publish frequent state to the announce topic. Keep docs and firmware defaults on `paradox/…`, not `site/…`.

## Other conventions

- Keep firmware docs (`docs/`) aligned with behaviour changes.
- **Prop admin reverse proxy:** HTTP UI honours `X-Forwarded-Prefix` / Host /
  Proto via `px-components/lib_http_proxy` (injects `<base href>`, `build_url`,
  `build_ws_url`). Client assets/API calls use path-relative URLs. See PxD
  `docs/PROP_ADMIN_REVERSE_PROXY.md`.

## Suite standards

Public suite brief + contracts live in [../../../apps/PxH/docs/standards/](../../../apps/PxH/docs/standards/) (folder, not a single file) — especially `AI-INSTRUCTIONS.md` and `MQTT-CONTRACT.md`. Read those before changing MQTT topics or shared conventions. If you change a standard, update the file under PxH `docs/standards/` first and propagate to other repos' docs in the same work.

If the workspace has `Px-Suite/` (or `/opt/paradox/Px-Suite`), use it for internal notes, cross-cutting pending plans, and business overview — do not put those into distributed PxH standards.

