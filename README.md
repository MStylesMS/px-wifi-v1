# px-wifi-v1

ESP32 Wi-Fi firmware — component of the Paradox escape-room system.

## License

Dual-licensed:

- **AGPL-3.0** for open source use — see [LICENSE](LICENSE).
- **Commercial license required** for proprietary or revenue-generating use that does not comply with AGPL-3.0 — see [COMMERCIAL.md](COMMERCIAL.md).

Copyright © 2026 Mark Stevens.

## Documentation

### For Developers
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
