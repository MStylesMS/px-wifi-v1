# Changelog

All notable changes to px-wifi-v1 are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Version numbers correspond to the contents of `version.txt`, which ESP-IDF
embeds into the firmware build (`esp_app_desc_t.version`).

## [Unreleased]

### Security / Fixed

- `prop_engine.c`/`.h`: added `prop_engine_get_battery_snapshot()`, a
  direct-field getter for the handful of scalars (`wireCount`,
  `batteryAdcRaw`, `batteryAdcAt0V`, `batteryAdcAt15V`, `batteryProfile`)
  that `web_ui.c`'s `mqtt_publish_announce()` needed. It previously called
  `prop_engine_get_config_json()` into a 2KB stack buffer and parsed those
  same values back out of the resulting JSON, just to avoid a getter —
  wasteful and, combined with the function's other ~1KB `announce` buffer,
  left `mqtt_publish_announce()` using ~3KB of stack on the esp-mqtt
  client's own (unsized-by-us) task. Removing the JSON round-trip cuts
  that to ~1KB.

- `web_ui.c`: `apply_connection_fields_from_json` now trims leading/trailing
  whitespace from `wifiPassword` before validating/saving it. A stray space
  (e.g. from a phone keyboard's autocapitalize/autocorrect, or a copy-paste)
  previously associated with the AP fine but silently failed the WPA2
  4-way handshake with no indication the password itself was the problem.
- `web_ui.c`: replaced `ESP_ERROR_CHECK()` (which aborts/reboots the whole
  device) in `command_post_handler`/`config_post_handler`/
  `config_restore_post_handler`/`ws_handler` with proper `esp_err_t`
  handling — an attacker-controlled or malformed request/command can no
  longer crash the prop; a busy engine now returns HTTP 503 / a WS error
  reply instead.
- `main.c`: `init_wire_gpio_interrupts()` now installs interrupts on the
  correct wire input GPIOs (`4,5,6,7,15,16,17,18`, matching
  `prop_engine.c`'s `s_wire_input_gpios[]` and
  `docs/pin-mapping.md`) instead of a stale/incorrect pin list.
- `web_ui.c`: `connection_scan_get_handler` now JSON-escapes scanned SSIDs
  before embedding them in the response, closing a JSON-injection hole
  where a rogue AP could broadcast an SSID containing `"`/control
  characters to corrupt the scan response or inject fields.
- `web_ui.c`: `config_post_handler` now applies WiFi credential changes
  (via the same validated path as `/api/connection`) and reconnects STA
  when the SSID/password change, instead of silently accepting but never
  applying new WiFi credentials submitted through `/api/config`.
- `web_ui.c`: `device_name_post_handler` now persists the updated network
  name to NVS/SPIFFS immediately instead of only holding it in RAM (lost
  on reboot).
- `web_ui.c`: all reads/writes of the shared `s_conn_cfg` connection
  config are now protected by a mutex (`s_conn_cfg_mutex`,
  `conn_cfg_lock`/`unlock`/`snapshot`) instead of being accessed
  unsynchronized from the HTTP server task, MQTT client task, and WiFi
  event callbacks concurrently.
- `web_ui.c`: MQTT inbound command/game-state-follower handling is now
  offloaded from `mqtt_on_message` (the MQTT client's own event task) to
  a dedicated `mqtt_inbound_worker` task via a queue, so slow command
  processing/publishes can no longer stall MQTT keepalive/reconnect.
- `web_ui.c`: `/api/config` and `/api/connection` POST handlers now share
  a single `apply_connection_fields_from_json()` helper for validating
  and applying WiFi/MQTT/network-name/AP fields, removing the previous
  duplicated (and inconsistent) logic between the two endpoints.
- SoftAP is now left enabled even after the device connects to a local
  WiFi network (previously it could be disabled via `apEnabled`/config).
  The AP's WPA2 PSK is relied on to keep it from being accessed by
  unauthorized parties; `apply_connection_fields_from_json` now forces
  `ap_enabled = true` and ignores client attempts to disable it.
- `PX_COMPONENTS_VERSION` bumped to 0.81 (see px-components CHANGELOG for
  the `svc_mqtt_publish` fix below).
- `web_ui.c`: all 6 previously fire-and-forget `svc_mqtt_publish()` call
  sites now go through a new `mqtt_publish_or_warn()` wrapper that logs a
  warning when the publish is dropped, completing the caller side of the
  `svc_mqtt_publish()` return-value fix (px-components v0.81) — the
  component-side fix alone was a no-op here since nothing checked the
  return value.
- `CONFIG_ESP_MAIN_TASK_STACK_SIZE` raised from the ESP-IDF default
  (3584) to 8192 in `sdkconfig.defaults`. `web_ui_start()` runs
  synchronously on the "main" task during boot and its locals (a 4KB
  `prop_cfg` buffer, `wifi_config_t`, connection-config snapshots, etc.)
  overflowed the default stack right as `esp_wifi_init()` added its own
  frames on top, causing a boot loop.

### Changed


- `prop_engine.c`: retuned built-in `6v-lead-acid` and `12v-lead-acid`
  voltage→capacity curves for light continuous load (ESP32 + roughly half
  the LEDs on), rather than open-circuit resting voltage. Full is now
  6.40 V / 12.80 V and empty is 5.75 V / 11.50 V, with a smoother mid-band
  that better matches expected prop runtime under light load.
- Low-battery deep sleep now powers down the RGB status LED(s), HT16K33
  7-segment display (blank + oscillator standby), and buzzer before
  `esp_deep_sleep_start()`, so they do not keep drawing current while the
  MCU sleeps. `main.c` registers a prepare hook via
  `prop_engine_set_deep_sleep_prepare_handler()`.

### Fixed

- `prop_engine.c`: raised `BATTERY_USB_ONLY_THRESHOLD_MV` from 400 to 2500 mV
  and made `timer_task()` skip both the zero-percent and low-battery-cutoff
  deep-sleep triggers whenever `battery_state == BATTERY_STATE_USB`. Running
  on USB power alone (no battery pack connected) previously read as a 0%
  battery, which satisfied the low-battery cutoff after 15s and forced the
  device into deep sleep shortly after boot — killing the SoftAP before a
  phone could discover/join it, and (if a wake GPIO was floating) presenting
  as a fast reset loop. No real battery voltage sags anywhere near 2.5V while
  still connected, so a reading below that reliably means "no pack attached."
- `prop_engine.c`: `init_battery_adc()` now also configures
  `drv_battery_monitor`'s median-of-5 pre-filter and a sustained-drop
  override (px-components v0.8) — snaps the filtered battery reading
  immediately to the recent average if it stays >=1V below the current
  value for a full continuous second, instead of creeping down via the
  EMA. `update_battery_runtime_unlocked()` keeps the override's threshold
  in sync with the live voltage calibration. This was driven by a data
  collection + analysis pass (see
  `docs/experiments/power/battery-adc-noise-2026-07-09.md`) that found
  the dominant noise source was brief, one-directional impulse dropouts
  (likely current-draw sag from nearby radio/PWM activity) rather than
  symmetric noise, and validated the fix against real captured data
  before applying it: steady-state jitter reduced from ~34mV to ~14mV
  peak-to-peak, while power-loss reaction time improved from ~2.85s to
  ~1.1s (both measured against captured device data, well within the
  <=5s requirement).
- `PX_COMPONENTS_VERSION` bumped to 0.8 for the above.

### Reverted

- Removed the short-lived `CONFIG_BOARD_DEVKITC1_V11` sdkconfig option:
  closer inspection showed this v2 (dual USB-C) devkit is an off-brand
  board whose WS2812 status LED is on GPIO48, same as the original board,
  not GPIO38 as on genuine Espressif DevKitC-1 v1.1 hardware. The Kconfig
  option and `board_devkitc1/include/board.h` `#ifdef` added for it have
  been reverted; the board is listed simply as the original DevKitC-1
  (GPIO48) again.

### Added

- `docs/experiments/power/battery-adc-noise-2026-07-09.md`: write-up of
  the battery ADC noise investigation — methodology, captured data,
  candidate filters tested, graphs, and the reasoning behind the final
  fix. Kept for future reference (e.g. if similar jitter is seen on
  other props, or the board is later revised to address the underlying
  current-draw-sag hardware behavior).

### Fixed

- `prop_engine.c`: `init_battery_adc()` now configures
  `BATTERY_ADC_OVERSAMPLE_COUNT` (16) via `drv_battery_monitor`'s new
  oversampling support (px-components v0.7), averaging 16 raw ADC reads
  per ~100ms sample instead of trusting a single one-shot read. Combined
  with the lower EMA alpha above, this further reduces the displayed
  battery voltage/percent jumping around during normal charging/running,
  by cutting noise at the source rather than only via a slower filter.
- `PX_COMPONENTS_VERSION` bumped to 0.7 for the above.

### Security

- `web_ui.c`: `config_post_handler`/`connection_post_handler` now reject
  request bodies above 16KB before allocating a buffer sized directly from
  the untrusted `Content-Length` header, closing a remote heap-exhaustion
  DoS.

### Fixed

- `prop_engine.c`: replaced the hand-rolled `json_extract_string/int/bool`
  parsers (raw pointer scanning, no escape-sequence handling) with thin
  wrappers around the shared, bounds-checked `lib_json_helper` (px-components
  v0.6). All ~30 call sites are unaffected since the function signatures are
  unchanged.
- `prop_engine.c`: all internal `xSemaphoreTake(s_ctx.lock, portMAX_DELAY)`
  calls replaced with a new bounded `prop_lock()`/`prop_unlock()` helper
  (5s timeout) so a stalled task can no longer deadlock the whole prop
  indefinitely; callers now handle a lock-timeout by skipping/aborting
  their current operation and logging an error.
- `prop_engine.c`: `save_config_file()`/`save_battery_file()` now check the
  return values of `fprintf()`/`fclose()` and return `ESP_FAIL` instead of
  silently persisting a possibly-truncated config on a full filesystem.
- `web_ui.c`: `mqtt_apply_follower_payload()` now logs a warning when an
  incoming MQTT follower message is truncated instead of silently dropping
  the tail.
- `main.c`: the lid-switch GPIO read in `lid_forces_blank()` now uses a
  named `LID_SWITCH_GPIO` constant (cross-referenced with `prop_engine.c`'s
  wire input table) instead of a bare `GPIO_NUM_18`.

### Added

- `docs/potential-improvements.md`: tracks security/architecture findings
  that were deliberately deferred during this review (no auth on the HTTP/
  WebSocket API, no OTA signature verification, plaintext WiFi/MQTT
  credentials on SPIFFS) along with the reasoning and a suggested approach
  for each, so future reviews don't re-report them as new findings.

### Changed

- `prop_engine.c`: lowered `BATTERY_ADC_EMA_ALPHA` from `0.15` to `0.04`.
  The battery ADC is sampled every ~100ms; at the old alpha the filter's
  real-time settling constant was only ~0.7s, which reacted fast to a
  genuine power-loss drop but also passed through much more ADC/charging
  ripple noise, making the displayed voltage/percent visibly jump around
  during normal operation. At 0.04 the settling constant is ~2.5s
  (~95% settled in ~7-8s) — still comfortably faster than the existing
  sustained-low-reading safety windows before deep sleep triggers
  (`LOW_BATTERY_CUTOFF_DELAY_MS` = 15s, `battery_shutdown_delay_s` default
  = 60s), so real power loss is still caught well within those grace
  periods, while normal charging/running readings are much smoother.
- `PX_COMPONENTS_VERSION` bumped to 0.6 (see px-components CHANGELOG for
  the `svc_wifi`/`svc_mqtt`/`drv_rgb_led` thread-safety and error-handling
  fixes this brings in).
- Added inline comments explaining the battery-voltage-divider calibration
  math (`battery_voltage_mv_from_adc_raw()` and friends in `prop_engine.c`)
  and the 7-segment display rendering helpers (`display_render_mmss()`,
  `display_apply_progress_bars()` in `main.c`); no behavior change.
- OTA firmware upload (`ota_upload_post_handler`) now delegates to the
  shared `svc_ota` component (px-components v0.5) via a small
  `httpd_req_recv` read-callback adapter, instead of driving
  `esp_ota_ops` directly.
- Connection-config JSON file load/save (`web_ui_json.c`) now uses the
  shared `svc_nvs_config` component (px-components v0.4) for file I/O and
  object merging instead of duplicating that logic inline. `prop_engine.c`'s
  own config/battery-profile file I/O (plain `fprintf`/`fread`, not
  cJSON-based) was left as-is — different enough in shape that converting
  it carried more risk than value for now.
- Battery ADC sampling + EMA smoothing moved to the shared
  `drv_battery_monitor` component (px-components v0.3). `prop_engine.c`
  keeps all voltage-divider math, chemistry-profile percent calculation,
  and low-battery cutoff behavior (calibration-dependent, prop-specific).
  No behavior change intended.
- Refactored WiFi/MQTT/JSON-helper code out of `web_ui.c` / `web_ui_json.c`
  and into the shared `px-components` library (`svc_wifi`, `svc_mqtt`,
  `lib_json_helper` — px-components v0.2). No behavior change intended;
  `web_ui.c` now calls through the new component APIs instead of touching
  `esp_wifi`/`esp_mqtt_client`/mDNS directly. Pending on-device OTA
  validation before the next version bump.
- Added `PX_COMPONENTS_VERSION` to record the px-components release this
  project is built/validated against.

## [0.1] - 2026-07-08

### Added

- `version.txt` as the single source of truth for the firmware version,
  read automatically by the ESP-IDF build (previously the version shown in
  `esp_app_desc_t` was just the `git describe` output).
- Battery ADC sampling (`adc_oneshot` on GPIO9 / ADC1_CH8) with an
  exponential moving average filter, sampled every ~100ms.
- Battery state detection: `normal`, `usb` (no battery/divider signal
  present), and `charging` (voltage above the profile's full-charge point).
  Exposed via the prop state JSON (`batteryState`) and the device details
  API (`batteryState`, `batteryVoltageMv`, `lowBattery`).
- Global RGB status LED brightness scaling so LED hints render at a
  consistent, dimmer overall brightness.

### Changed

- `external`/`unknown` battery profile full-charge point raised from
  5000 mV to 5250 mV to better match measured hardware.
