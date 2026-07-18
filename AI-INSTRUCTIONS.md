# px-wifi-v1 — AI Instructions

ESP32 Wi-Fi smart-prop firmware for Paradox escape rooms.

## Conventions

- MQTT: `{baseTopic}/{commands|events|state|warnings}` (`/warnings` plural).
- One-shot announce on connect to `paradox/props` (suite standard; avoid `site/props`).
- Keep firmware docs (`docs/`) aligned with behaviour changes.
