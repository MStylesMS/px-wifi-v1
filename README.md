# px-wifi-v1

ESP32 Wi-Fi firmware — component of the Paradox escape-room system.

## License

Dual-licensed:

- **AGPL-3.0** for open source use — see [LICENSE](LICENSE).
- **Commercial license required** for proprietary or revenue-generating use that does not comply with AGPL-3.0 — see [COMMERCIAL.md](COMMERCIAL.md).

Copyright © 2026 Mark Stevens.

## ESP-IDF

Pin **ESP-IDF 6.0.x** (stable 6.0.3 or newer 6.0 bugfix). Same IDF as
`../px-components` (`PX_COMPONENTS_VERSION`). `main/idf_component.yml` requires
`idf: ">=6.0.0"`.

```powershell
# After export.ps1 for IDF 6.0.x:
idf.py set-target esp32s3
idf.py build
```

Do not flash venue hardware until a human says so.

## Documentation

### For Developers
- [**Prop console chrome**](docs/console-chrome.md) — Live / Config / Connect page contract, brand tokens, local UI preview
- [**Hardware Specification**](docs/hardware-spec.md) — GPIO pinout, power requirements, battery system
- [**Functional Specification**](docs/functional-spec.md) — Feature requirements and behavior
- [**API Reference**](docs/api.md) — HTTP endpoint documentation

### For Integrators & Battery Commissioning
- [**Battery Voltage Calibration Guide**](docs/battery-calibration.md) — How to calibrate ADC for accurate voltage/charge readings ⚠️ **Important for production deployment**
- [**User Guide**](docs/user-guide.md) — End-user instructions and operation

### Analysis & Reference
- [**ADC Calibration Analysis Report**](docs/experiments/adc-calibration-analysis.html) — Interactive visualization of voltage calibration curve fit and error analysis (open in browser)
- [**Pin Mapping**](docs/pin-mapping.md) — GPIO assignments for all peripherals
- [**Product Brief**](docs/product-brief.md) — High-level overview
