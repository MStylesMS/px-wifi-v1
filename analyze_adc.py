import json
import math

# Device constants
R1 = 21600
R2 = 4430
BATTERY_ADC_FULL_SCALE_MV = 15000
ADC_AT_0V = 0
ADC_AT_15V = 3020

# Calculate sense reference voltage
sense_ref_mv = BATTERY_ADC_FULL_SCALE_MV * R2 / (R1 + R2)
print(f"sense_ref_mv = {sense_ref_mv:.2f} mV\n")

# Your data: MM (multimeter) vs UI (device reported)
data = [
    (7.87, 7.90),
    (6.41, 6.27),
    (6.25, 6.00),
    (6.20, 5.91),
    (6.19, 5.68),
    (12.25, 12.45),
    (6.45, 6.14),
]

# Back-calculate ADC raw values from UI voltage
def ui_to_adc(ui_voltage_v):
    ui_voltage_mv = ui_voltage_v * 1000
    sense_mv = ui_voltage_mv * R2 / (R1 + R2)
    adc_raw = ADC_AT_0V + (sense_mv * (ADC_AT_15V - ADC_AT_0V) / sense_ref_mv)
    return adc_raw

# Extract arrays and calculate ADC from UI
mm_voltages = np.array([d[0] for d in data])
ui_voltages = np.array([d[1] for d in data])
adc_from_ui = np.array([ui_to_adc(v) for v in ui_voltages])

print("Data points (with back-calculated ADC):")
print("MM (V) | UI (V) | UI→ADC | Error(UI-MM)")
print("-" * 50)
for i, (mm, ui) in enumerate(data):
    adc = adc_from_ui[i]
    error = ui - mm
    print(f"{mm:6.2f} | {ui:6.2f} | {adc:6.1f} | {error:+6.2f}%")

# Linear regression: MM vs ADC
slope_mm, intercept_mm, r_mm, p_mm, se_mm = stats.linregress(adc_from_ui, mm_voltages * 1000)
print(f"\n\n=== MM vs ADC (Ground Truth) ===")
print(f"Slope: {slope_mm:.6f} mV/count")
print(f"Intercept: {intercept_mm:.2f} mV")
print(f"R²: {r_mm**2:.6f}")
print(f"Std Error: {se_mm:.6f}")

# Linear regression: UI vs ADC
slope_ui, intercept_ui, r_ui, p_ui, se_ui = stats.linregress(adc_from_ui, ui_voltages * 1000)
print(f"\n=== UI vs ADC (Device Reported) ===")
print(f"Slope: {slope_ui:.6f} mV/count")
print(f"Intercept: {intercept_ui:.2f} mV")
print(f"R²: {r_ui**2:.6f}")
print(f"Std Error: {se_ui:.6f}")

# Compute optimal ADC calibration from MM regression
# The regression gives us: V_mm = slope_mm * adc + intercept_mm
# We want: V = (adc - adc_at_0v) * sense_ref_mv / adc_at_15v * (R1+R2)/R2
# If we set adc_at_0v to match intercept and solve for adc_at_15v:
optimal_adc_at_0v = max(0, int(round(intercept_mm / slope_mm)))
optimal_adc_at_15v = int(round((BATTERY_ADC_FULL_SCALE_MV * sense_ref_mv) / (slope_mm * (R1 + R2) / R2)))

print(f"\n\n=== Proposed Optimal Settings ===")
print(f"Recommended adcAt0V: {optimal_adc_at_0v}")
print(f"Recommended adcAt15V: {optimal_adc_at_15v}")

# Calculate errors with proposed settings
def voltage_from_adc_proposed(adc_raw):
    sense_mv = ((adc_raw - optimal_adc_at_0v) * sense_ref_mv) / (optimal_adc_at_15v - optimal_adc_at_0v)
    voltage_mv = sense_mv * (R1 + R2) / R2
    return voltage_mv / 1000

errors_proposed = []
print(f"\n\n=== Error Analysis with Proposed Settings ===")
print("MM (V) | Calculated (V) | Error (V) | Error (%)")
print("-" * 55)
max_abs_error = 0
for i, mm in enumerate(mm_voltages):
    calc = voltage_from_adc_proposed(adc_from_ui[i])
    error_v = calc - mm
    error_pct = (error_v / mm) * 100
    errors_proposed.append(error_v)
    max_abs_error = max(max_abs_error, abs(error_pct))
    print(f"{mm:6.2f} | {calc:6.2f}      | {error_v:+7.3f} | {error_pct:+6.2f}%")

mean_abs_error = np.mean(np.abs(errors_proposed))
print(f"\nMean Absolute Error: {mean_abs_error:.3f} V ({mean_abs_error/np.mean(mm_voltages)*100:.2f}%)")
print(f"Max Absolute Error: {max_abs_error:.2f}%")

# Output for HTML visualization
output = {
    "data": [
        {"mm": float(mm), "ui": float(ui), "adc": float(adc)} 
        for mm, ui, adc in zip(mm_voltages, ui_voltages, adc_from_ui)
    ],
    "current_settings": {"adc_at_0v": ADC_AT_0V, "adc_at_15v": ADC_AT_15V},
    "proposed_settings": {"adc_at_0v": optimal_adc_at_0v, "adc_at_15v": optimal_adc_at_15v},
    "regression": {
        "mm": {"slope": float(slope_mm), "intercept": float(intercept_mm), "r2": float(r_mm**2)},
        "ui": {"slope": float(slope_ui), "intercept": float(intercept_ui), "r2": float(r_ui**2)}
    },
    "error_metrics": {
        "mean_abs_error_v": float(mean_abs_error),
        "mean_abs_error_pct": float(mean_abs_error/np.mean(mm_voltages)*100),
        "max_abs_error_pct": float(max_abs_error)
    }
}

print(json.dumps(output, indent=2))
