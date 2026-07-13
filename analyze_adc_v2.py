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

# Linear regression helper
def linear_regression(x_vals, y_vals):
    n = len(x_vals)
    sum_x = sum(x_vals)
    sum_y = sum(y_vals)
    sum_xy = sum(x * y for x, y in zip(x_vals, y_vals))
    sum_x2 = sum(x * x for x in x_vals)
    
    slope = (n * sum_xy - sum_x * sum_y) / (n * sum_x2 - sum_x * sum_x)
    intercept = (sum_y - slope * sum_x) / n
    
    # Calculate R²
    y_mean = sum_y / n
    ss_tot = sum((y - y_mean) ** 2 for y in y_vals)
    ss_res = sum((y - (slope * x + intercept)) ** 2 for x, y in zip(x_vals, y_vals))
    r2 = 1 - (ss_res / ss_tot) if ss_tot != 0 else 0
    
    # Standard error
    residuals = [(y - (slope * x + intercept)) for x, y in zip(x_vals, y_vals)]
    se = math.sqrt(sum(r * r for r in residuals) / (n - 2)) if n > 2 else 0
    
    return slope, intercept, r2, se

# Extract arrays and calculate ADC from UI
mm_voltages = [d[0] for d in data]
ui_voltages = [d[1] for d in data]
adc_from_ui = [ui_to_adc(v) for v in ui_voltages]

print("Data points (with back-calculated ADC):")
print("MM (V) | UI (V) | UI→ADC | Error(UI-MM)")
print("-" * 50)
for i, (mm, ui) in enumerate(data):
    adc = adc_from_ui[i]
    error = ui - mm
    print(f"{mm:6.2f} | {ui:6.2f} | {adc:6.1f} | {error:+6.2f}V")

# Linear regression: MM vs ADC (in mV for consistency)
mm_voltages_mv = [v * 1000 for v in mm_voltages]
slope_mm, intercept_mm, r2_mm, se_mm = linear_regression(adc_from_ui, mm_voltages_mv)
print(f"\n\n=== MM vs ADC (Ground Truth) ===")
print(f"Slope: {slope_mm:.6f} mV/count")
print(f"Intercept: {intercept_mm:.2f} mV")
print(f"R²: {r2_mm:.6f}")
print(f"Std Error: {se_mm:.6f}")

# Linear regression: UI vs ADC
ui_voltages_mv = [v * 1000 for v in ui_voltages]
slope_ui, intercept_ui, r2_ui, se_ui = linear_regression(adc_from_ui, ui_voltages_mv)
print(f"\n=== UI vs ADC (Device Reported) ===")
print(f"Slope: {slope_ui:.6f} mV/count")
print(f"Intercept: {intercept_ui:.2f} mV")
print(f"R²: {r2_ui:.6f}")
print(f"Std Error: {se_ui:.6f}")

# Compute optimal ADC calibration from MM regression
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
max_abs_error_pct = 0
for i, mm in enumerate(mm_voltages):
    calc = voltage_from_adc_proposed(adc_from_ui[i])
    error_v = calc - mm
    error_pct = (error_v / mm) * 100 if mm != 0 else 0
    errors_proposed.append(error_v)
    max_abs_error_pct = max(max_abs_error_pct, abs(error_pct))
    print(f"{mm:6.2f} | {calc:6.2f}      | {error_v:+7.3f} | {error_pct:+6.2f}%")

mean_abs_error = sum(abs(e) for e in errors_proposed) / len(errors_proposed)
mean_voltage = sum(mm_voltages) / len(mm_voltages)
print(f"\nMean Absolute Error: {mean_abs_error:.3f} V ({mean_abs_error/mean_voltage*100:.2f}%)")
print(f"Max Absolute Error: {max_abs_error_pct:.2f}%")

# Output for HTML visualization
output = {
    "data": [
        {"mm": mm, "ui": ui, "adc": adc} 
        for mm, ui, adc in zip(mm_voltages, ui_voltages, adc_from_ui)
    ],
    "current_settings": {"adc_at_0v": ADC_AT_0V, "adc_at_15v": ADC_AT_15V},
    "proposed_settings": {"adc_at_0v": optimal_adc_at_0v, "adc_at_15v": optimal_adc_at_15v},
    "regression": {
        "mm": {"slope": slope_mm, "intercept": intercept_mm, "r2": r2_mm},
        "ui": {"slope": slope_ui, "intercept": intercept_ui, "r2": r2_ui}
    },
    "error_metrics": {
        "mean_abs_error_v": mean_abs_error,
        "mean_abs_error_pct": mean_abs_error/mean_voltage*100,
        "max_abs_error_pct": max_abs_error_pct
    }
}

print("\n\n=== JSON OUTPUT FOR VISUALIZATION ===")
print(json.dumps(output, indent=2))
