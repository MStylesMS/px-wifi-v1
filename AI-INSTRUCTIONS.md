# px-wifi-v1 — AI Instructions

ESP32 Wi-Fi smart-prop firmware for Paradox escape rooms.

## Conventions

- MQTT: `{baseTopic}/{commands|events|state|warnings}` (`/warnings` plural).
- One-shot announce on connect to `paradox/props` (suite standard; avoid `site/props`).
- Keep firmware docs (`docs/`) aligned with behaviour changes.
- **Prop admin reverse proxy:** HTTP UI honours `X-Forwarded-Prefix` via
  `px-components/lib_http_proxy` (injects `<base href>` into HTML). Client
  assets/API calls use path-relative URLs. See PxD `docs/PROP_ADMIN_REVERSE_PROXY.md`.
