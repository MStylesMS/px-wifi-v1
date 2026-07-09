# Changelog

All notable changes to px-wifi-v1 are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Version numbers correspond to the contents of `version.txt`, which ESP-IDF
embeds into the firmware build (`esp_app_desc_t.version`).

## [Unreleased]

### Changed

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
