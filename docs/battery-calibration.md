# Battery Voltage Calibration & Tuning Guide

## Overview

The PX-WiFi-V1 device measures battery voltage using an analog-to-digital converter (ADC) with a resistive voltage divider. Accurate calibration is critical for:
- **Accurate state-of-charge (SoC) estimation** — Small voltage errors amplify into large charge percentage errors due to the non-linear discharge curve of sealed lead-acid (SLA) batteries
- **Reliable deep-sleep triggering** — The device enters deep sleep at 20% SoC (~5.4V) to protect the battery; incorrect calibration causes premature or delayed shutdowns
- **Battery life optimization** — Proper calibration allows the device to use the full usable capacity without false low-battery alerts

---

## Hardware Specifications

### Resistive Voltage Divider
- **R1 (high side):** 21,600Ω
- **R2 (low side):** 4,430Ω  
- **Voltage reference:** 15V max input maps to 12-bit ADC full scale (0–4095 counts)
- **Divider ratio:** 5.869 (converts measured voltage to sense voltage at ADC input)
- **Divider formula:** `Sense_mV = Measured_V_mV × R2 / (R1 + R2)`

### ADC Characteristics
- **Resolution:** 12-bit (0–4095 counts)
- **Reference:** Internal 3.3V
- **Full-scale sense voltage:** 2552.82 mV (this is the voltage at R2 when input is 15V)
- **Calibration offsets:** `adcAt0V` and `adcAt15V` define the linear mapping from ADC counts to voltage

---

## Calibration Formula

The firmware converts ADC raw counts to voltage using:

```
voltage_mv = (adc_raw - adcAt0V) × sense_ref_mv / (adcAt15V - adcAt0V)
voltage_v = voltage_mv / 1000
```

Where:
- `adc_raw`: Raw ADC reading (0–4095 counts)
- `adcAt0V`: ADC count corresponding to 0V output (can be negative)
- `adcAt15V`: ADC count corresponding to 15V output
- `sense_ref_mv`: 2552.82 mV (fixed divider reference)

### Key Insight
- `adcAt0V` and `adcAt15V` define a **linear transformation** from ADC counts to voltage
- The optimal values are determined by **linear regression** on ground-truth measurements
- Negative `adcAt0V` values are valid and represent ADC offsets below zero; they're common in practical systems with real circuit tolerances

---

## Battery Discharge Curve (6V Sealed Lead-Acid Reference)

The following open-circuit voltage (OCV) points define a typical 6V SLA battery discharge curve:

| Voltage | SoC % | Description |
|---------|-------|-------------|
| 6.30V   | 100%  | Fully charged |
| 6.00V   | 80%   | Good capacity |
| 5.80V   | 60%   | Mid-range |
| 5.60V   | 40%   | Declining |
| **5.40V** | **20%** | **⚠️ Deep-sleep cutoff** |
| 5.00V   | 0%    | Fully discharged |

### Critical Observation
The discharge curve is **non-linear**:
- From 6.3V → 5.4V, voltage drops only 0.9V but SoC drops 80 percentage points
- A **0.2V measurement error in this region = ~20% SoC error**
- This is why small voltage calibration errors cause catastrophic SoC estimation failures

---

## Calibration Procedure (For End Users)

### Requirements
- Multimeter (DC voltage measurement, ±0.05V accuracy or better)
- Access to device web UI (HTTP API)
- Battery in steady-state (no charging, 30+ minutes idle)

### Step 1: Measure Reference Point(s)

**Measure battery voltage with multimeter:**
1. Disconnect the device's load
2. Let the battery stabilize for 30 minutes
3. Measure voltage between battery terminals
4. Record the reading (e.g., 6.41V)

**Recommended:** Take 2-3 measurements at different discharge states (e.g., after power-down, after a few hours idle, after a day idle).

### Step 2: Read Device Voltage

1. Open the web UI or use curl: `curl http://<device-ip>/api/state`
2. Note the `batteryVoltageMv` value
3. Convert to volts: 6270 mV = 6.27V

### Step 3: Back-Calculate ADC Raw Value

Using the current calibration settings (retrieve from `/api/config`), calculate the ADC raw count that produced the device reading:

```python
adc_raw = (ui_voltage_mv / 15000) × (adcAt15V - adcAt0V) + adcAt0V
```

Example:
- UI voltage: 6.27V = 6270 mV
- Current: adcAt0V=0, adcAt15V=3020
- ADC: (6270 / 15000) × 3020 + 0 = 1262.4 counts

### Step 4: Perform Linear Regression (If Multiple Points)

If you have N measurement pairs (multimeter, device):
1. Back-calculate ADC for each device reading
2. Fit a linear model: `V_mm = slope × adc + intercept`
3. Solve for optimal calibration:
   - `optimal_adcAt0V = -intercept / slope`
   - `optimal_adcAt15V = optimal_adcAt0V + (15000 / slope)`

**Recommended tool:** Use Python with NumPy/SciPy, or online linear regression calculator with (ADC, multimeter voltage) pairs.

### Step 5: Apply New Settings

```bash
curl -X POST http://<device-ip>/api/config \
  -H "Content-Type: application/json" \
  -d '{
    "batteryAdcAt0V": <value>,
    "batteryAdcAt15V": <value>,
    "lowBatteryCutoffPercent": 20
  }'

curl -X POST http://<device-ip>/api/config/save
```

### Step 6: Verify

Measure battery voltage again with multimeter and compare to device reading:
- Should agree within ±0.1V (±2% error)
- If error is larger, repeat with more data points

---

## Current Calibration (PX-WiFi-V1 as of 2026-07-11)

**Optimal settings determined via 7-point linear regression analysis:**

```json
{
  "batteryAdcAt0V": -172,
  "batteryAdcAt15V": 3124,
  "lowBatteryCutoffPercent": 20
}
```

### Performance Metrics
- **Measurement range:** 6.0V–12.5V
- **Mean absolute error:** 0.086V (1.17% of voltage)
- **Max error:** 3.28% (occurs at 6.19V)
- **R² fit:** 0.9972 (excellent correlation)
- **SoC error reduction:** 12% improvement over previous uncalibrated settings

### Commissioning Story
The device was entering deep sleep ~16 seconds after boot on battery power due to an uncalibrated ADC (adcAt0V=0, adcAt15V=3020 without linear regression). The ADC was reading 6.19V as 5.68V (48% SoC instead of actual 93%), falsely triggering the 20% cutoff. After linear regression tuning, the same 6.19V now reads as 79.7% SoC, allowing the device to operate for weeks on a 6V 4.5Ah battery.

---

## Troubleshooting

### Device keeps entering deep sleep prematurely
- **Likely cause:** ADC is underreading voltage (too negative offset or slope too small)
- **Fix:** Increase `adcAt15V` or decrease `adcAt0V` (make it more negative)
- **Verification:** Check device voltage reading vs. multimeter; if device reads lower, calibration is off

### Device doesn't enter deep sleep even when battery is nearly dead
- **Likely cause:** ADC is overreading voltage (offset too large or slope too steep)
- **Fix:** Decrease `adcAt15V` or increase `adcAt0V`

### Voltage readings fluctuate wildly
- **Likely cause:** Load is not stable (charger connected, high-current draw)
- **Fix:** Disconnect load, wait 30+ minutes, re-measure

### Calibration works at 6V but fails at 12V (for 12V battery)
- **Likely cause:** Linear model is insufficient; battery has non-linear ADC response
- **Fix:** Add more calibration points in the 5–12V range and re-run regression

---

## References

- [Linear Regression Analysis Report](../experiments/adc-calibration-analysis.html) — Interactive visualization of the calibration curve fit and per-point error analysis
- Battery: Typical 6V/12V sealed lead-acid (SLA) specifications per IEC 61056-1
- ADC: ESP32-S3 built-in SAR ADC, 12-bit resolution
- Divider tolerance: Resistors are 1% tolerance; for precision, measure actual resistor values and recalibrate if needed

---

## Custom Battery Capacity Curves

The device supports customizing battery discharge curves for different battery types or specific packs. Curves are defined as voltage-to-capacity lookup points.

### Adding a Custom Curve via REST API

If you need a curve for a non-standard battery (e.g., a specific LiFePO4 variant or measured discharge profile), you can upload it via the config API:

**1. Define your curve as voltage:percent pairs**

Measure your battery at multiple discharge points (minimum 4–5 points recommended):

| Voltage | Capacity |
|---------|----------|
| 6.40V   | 100%     |
| 6.20V   | 75%      |
| 6.00V   | 50%      |
| 5.80V   | 25%      |
| 5.50V   | 0%       |

**2. Send via REST API (POST /api/config)**

```bash
curl -X POST http://<device-ip>/api/config \
  -H "Content-Type: application/json" \
  -d '{
    "batteryProfile": "6v-lead-acid",
    "batteryPoints": "6.400:100,6.200:75,6.000:50,5.800:25,5.500:0"
  }'

curl -X POST http://<device-ip>/api/config/save
```

**3. Verify the curve was applied**

```bash
curl http://<device-ip>/api/state | grep battery
```

### Curve Format

Battery curves use **CSV notation**: `voltage_v:percent,voltage_v:percent,...`

- **voltage_v** is in volts (e.g., `6.400` = 6.4V)
- **percent** is the state-of-charge (0–100)
- Points should be in **descending voltage order** (highest voltage first)
- Minimum 2 points required; typically 5–8 points for good accuracy
- Device interpolates linearly between points

### Example: Custom 6V LiFePO4 Curve

LiFePO4 cells have a flatter discharge curve than lead-acid. A typical 6V LiFePO4 (two 3.2V cells in series) might look like:

```
"batteryPoints": "6.500:100,6.400:95,6.300:90,6.200:75,6.100:50,5.950:25,5.800:5,5.600:0"
```

### Example: Measured Field Data

If you've measured your specific battery pack under the expected operating load:

```
"batteryPoints": "6.350:100,6.200:85,6.000:70,5.800:50,5.600:30,5.400:15,5.200:0"
```

This overrides the selected builtin profile's points and persists across power cycles. Do not use the `external` or `unknown` profiles for a curve that should report a percentage; those profiles intentionally always report 100%.

### Switching Back to a Builtin Profile

If you want to return to the default profile:

```bash
curl -X POST http://<device-ip>/api/config \
  -H "Content-Type: application/json" \
  -d '{
    "batteryProfile": "6v-lead-acid"
  }'

curl -X POST http://<device-ip>/api/config/save
```

Valid builtin profiles: `6v-lead-acid`, `6v-LiFePO4`, `12v-lead-acid`, `12v-LiFePO4`, `external`, `unknown`

---

## Version History

- **2026-07-11 (v0.32+):** Linear regression calibration applied (adcAt0V=-172, adcAt15V=3124); 6V lead-acid profile updated to match standard OCV curve
- **2026-07-10 (v0.31):** Initial deep-sleep fixes; uncalibrated ADC (factory defaults)
- **2026-07-09 (v0.30):** Firmware feature parity with base system
