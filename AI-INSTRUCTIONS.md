# px-wifi-v1 — AI Instructions

ESP32 Wi-Fi smart-prop firmware for Paradox escape rooms.

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
- **Prop admin reverse proxy:** HTTP UI honours `X-Forwarded-Prefix` via
  `px-components/lib_http_proxy` (injects `<base href>` into HTML). Client
  assets/API calls use path-relative URLs. See PxD `docs/PROP_ADMIN_REVERSE_PROXY.md`.

## Suite standards

Suite-wide contracts live in [../../../apps/PxH/docs/standards/](../../../apps/PxH/docs/standards/) (folder, not a single file). Read those before changing MQTT topics or shared conventions. If you change a standard, update the file under PxH `docs/standards/` first and propagate to other repos' docs in the same work.
