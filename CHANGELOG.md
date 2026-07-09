# Changelog

All notable changes to px-wifi-v1 are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Version numbers correspond to the contents of `version.txt`, which ESP-IDF
embeds into the firmware build (`esp_app_desc_t.version`).

## [Unreleased]

### Fixed

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
