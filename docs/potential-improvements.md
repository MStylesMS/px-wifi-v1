# Potential Improvements — Deferred / Not Yet Implemented

This file tracks security- or architecture-significant findings that were
**deliberately deferred** rather than fixed immediately — usually because
they require a product/deployment decision (e.g. "do we need auth on the
LAN?"), a larger design effort, or hardware/process changes outside the
scope of a single code change. It complements (does not replace)
`CHANGELOG.md`, which tracks changes that *were* made.

Any AI assistant or contributor doing a security/code review of this
project should read this file first, to avoid re-reporting the same
findings as new discoveries, and should add newly-deferred findings here
using the same format instead of only mentioning them in chat/PR
descriptions.

## Format

Each entry: what/where, why it matters, why it was deferred, and a
concrete suggested approach for when it is picked up.

---

## 1. No authentication on HTTP/WebSocket API endpoints

- **Where:** `main/web_ui.c` — `command_post_handler`, `config_post_handler`,
  `connection_post_handler`, `ota_upload_post_handler`, `ws_handler`, and
  effectively every other route registered in `web_ui_start()`.
- **Why it matters:** Any device able to reach the prop's AP or venue LAN
  can start/stop/reset the game, change the wire solution/timing, change
  WiFi/MQTT credentials, or push new firmware (see #2), with zero
  credentials required.
- **Why deferred:** Requires a product decision on the auth model
  appropriate for an escape-room LAN deployment (shared PIN? per-device API
  key issued at setup time? mDNS/local-subnet-only restriction? client
  cert?) and corresponding UI/setup-flow changes in `webui/`, plus updates
  to `docs/api.md`. Not something to bolt on as a one-line patch.
- **Suggested approach when picked up:**
  - Add a per-device setup-time API key (generated on first boot, shown in
    the connection/config web UI) required via an `Authorization` header
    or query param for all mutating endpoints (`POST`/`ws` command
    messages). Read-only status endpoints (`/api/state`) can likely stay
    open for e.g. a game-master dashboard, or gate those too if the venue
    LAN can't be trusted.
  - At minimum, restrict `/api/ota/upload` more tightly than other
    endpoints (see #2) since its blast radius is highest.
  - Document the chosen scheme in `docs/api.md` and `docs/functional-spec.md`.

## 2. OTA firmware upload has no signature/version verification

- **Where:** `main/web_ui.c` — `ota_upload_post_handler()` →
  `svc_ota_apply()` in `px-components/svc_ota/svc_ota.c`.
- **Why it matters:** Combined with #1 (no auth), any device on the network
  can push arbitrary firmware to the prop. Even with #1 fixed, there's no
  defense against a compromised/careless operator uploading a corrupt or
  malicious build (no signature check, no minimum-version/rollback
  protection).
- **Why deferred:** Requires deciding on and implementing a firmware
  signing pipeline (keypair generation/storage, build-time signing step in
  the release process, signature verification before
  `esp_ota_write()`/`esp_ota_end()`), which is a build/release-process
  change as much as a firmware change, and needs to be coordinated across
  all prop firmware projects (not just `px-wifi-v1`), likely living in
  `px-components/svc_ota` once designed.
- **Suggested approach when picked up:**
  - Sign release builds (e.g. Ed25519 over the app image or a manifest
    referencing its hash) as part of the release/build script.
  - Extend `svc_ota` with a verification step that checks the signature
    against a public key baked into the firmware (or an eFuse-backed key)
    before finalizing the update, aborting via `esp_ota_abort()` on
    mismatch.
  - Consider also enabling ESP-IDF's anti-rollback / secure boot features
    for production hardware revisions.

## 3. WiFi/MQTT credentials stored in plaintext on SPIFFS

- **Where:** `/spiffs/config.json` (written by
  `save_connection_cfg_nvs()`/related code in `main/web_ui.c`), containing
  `wifiPassword`, `mqttPassword`, etc.
- **Why it matters:** Physical access to the device (or a SPIFFS image
  dump) exposes venue network credentials in cleartext.
- **Why deferred:** Explicitly deprioritized for now — these are
  physically-secured venue props, not general consumer/IoT devices, and
  the WiFi/MQTT credentials in question are scoped to the venue's own
  network. Revisit if props are deployed in less trusted physical
  environments, or before treating this config format as a stable/exported
  artifact.
- **Suggested approach when picked up:** Move credential storage to NVS
  with ESP32 flash encryption enabled, or encrypt just the credential
  fields at rest using a key derived from an eFuse/secure element rather
  than storing them as plain JSON text.

---

## How to use this file going forward

- When a code review (human or AI-assisted) identifies a finding that is
  explicitly deferred rather than fixed, add an entry here in the same
  commit/session, using the format above.
- Before starting a new security/code review pass, read this file so
  previously-deferred items are recognized as "known and tracked" rather
  than re-reported as new findings — cross-reference and update status
  here instead of duplicating.
- When a deferred item is eventually fixed, move its entry to
  `CHANGELOG.md` under the commit that fixes it and delete it from this
  file (or mark it "Resolved in vX.Y" and leave it for historical
  context — either is fine, just be consistent within a given file).
