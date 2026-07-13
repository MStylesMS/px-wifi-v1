import json
import math

# Device constants
R1 = 21600
R2 = 4430
BATTERY_ADC_FULL_SCALE_MV = 15000
sense_ref_mv = BATTERY_ADC_FULL_SCALE_MV * R2 / (R1 + R2)

# Current calibration
ADC_AT_0V_OLD = 0
ADC_AT_15V_OLD = 3020

# Proposed calibration
ADC_AT_0V_NEW = -172
ADC_AT_15V_NEW = 3124

# Your data
data = [
    (7.87, 7.90),
    (6.41, 6.27),
    (6.25, 6.00),
    (6.20, 5.91),
    (6.19, 5.68),
    (12.25, 12.45),
    (6.45, 6.14),
]

def ui_to_adc(ui_voltage_v):
    ui_voltage_mv = ui_voltage_v * 1000
    sense_mv = ui_voltage_mv * R2 / (R1 + R2)
    adc_raw = ADC_AT_0V_OLD + (sense_mv * (ADC_AT_15V_OLD - ADC_AT_0V_OLD) / sense_ref_mv)
    return adc_raw

def adc_to_voltage(adc_raw, adc_at_0v, adc_at_15v):
    """Convert ADC raw value to voltage using calibration settings"""
    sense_mv = ((adc_raw - adc_at_0v) * sense_ref_mv) / (adc_at_15v - adc_at_0v)
    voltage_mv = sense_mv * (R1 + R2) / R2
    return voltage_mv / 1000

def voltage_to_soc_6v_sla(voltage_v):
    """
    Convert voltage to state of charge for a typical 6V sealed lead-acid battery.
    Based on standard SLA discharge curves.
    
    Reference curve (open-circuit voltage):
    6.30V = 100%
    6.00V = 80%
    5.80V = 60%
    5.60V = 40%
    5.40V = 20%
    5.00V = 0% (dead)
    
    Using piecewise linear interpolation between known points.
    """
    points = [
        (6.30, 100),
        (6.00, 80),
        (5.80, 60),
        (5.60, 40),
        (5.40, 20),
        (5.00, 0),
    ]
    
    # Clamp to battery range
    if voltage_v >= points[0][0]:
        return 100.0
    if voltage_v <= points[-1][0]:
        return 0.0
    
    # Find the two points to interpolate between
    for i in range(len(points) - 1):
        v1, soc1 = points[i]
        v2, soc2 = points[i + 1]
        
        if v2 <= voltage_v <= v1:
            # Linear interpolation
            soc = soc1 + (soc2 - soc1) * (voltage_v - v1) / (v2 - v1)
            return soc
    
    return 0.0

print("=== Battery State of Charge (SoC) Analysis ===")
print("\nBattery Model: Typical 6V Sealed Lead-Acid (SLA)")
print("Reference discharge curve:")
print("  6.30V = 100% | 6.00V = 80% | 5.80V = 60% | 5.60V = 40% | 5.40V = 20% | 5.00V = 0%")
print("\n")

print("Data Point | MM Voltage | MM SoC | OLD UI Vol | OLD SoC | OLD Error | NEW UI Vol | NEW SoC | NEW Error | SoC Gain")
print("-" * 120)

soc_results = []
deep_sleep_cutoff_v = 5.40  # 20% for deep sleep

for idx, (mm, old_ui) in enumerate(data, 1):
    adc = ui_to_adc(old_ui)
    new_ui = adc_to_voltage(adc, ADC_AT_0V_NEW, ADC_AT_15V_NEW)
    
    mm_soc = voltage_to_soc_6v_sla(mm)
    old_soc = voltage_to_soc_6v_sla(old_ui)
    new_soc = voltage_to_soc_6v_sla(new_ui)
    
    old_error = old_soc - mm_soc
    new_error = new_soc - mm_soc
    soc_gain = abs(old_error) - abs(new_error)
    
    print(f"{idx:10d} | {mm:10.2f} | {mm_soc:5.1f}% | {old_ui:10.2f} | {old_soc:5.1f}% | {old_error:+7.1f}% | {new_ui:10.2f} | {new_soc:5.1f}% | {new_error:+7.1f}% | {soc_gain:+6.1f}%")
    
    soc_results.append({
        "index": idx,
        "mm_voltage": mm,
        "mm_soc": mm_soc,
        "old_ui_voltage": old_ui,
        "old_soc": old_soc,
        "old_error_pct": old_error,
        "new_ui_voltage": new_ui,
        "new_soc": new_soc,
        "new_error_pct": new_error,
        "soc_gain": soc_gain,
        "below_cutoff_old": old_ui < deep_sleep_cutoff_v,
        "below_cutoff_new": new_ui < deep_sleep_cutoff_v,
        "below_cutoff_mm": mm < deep_sleep_cutoff_v
    })

print("\n=== Critical Analysis: Deep-Sleep Threshold (20% SoC ≈ 5.40V) ===")
print("\nPoints where old calibration triggers false deep-sleep (UI < 5.40V but MM >= 5.40V):")
false_triggers = [r for r in soc_results if r["below_cutoff_old"] and not r["below_cutoff_mm"]]
if false_triggers:
    for r in false_triggers:
        actual_soc = r["mm_soc"]
        reported_soc = r["old_soc"]
        print(f"  Point {r['index']}: MM shows {actual_soc:.1f}% charge (OK), but device reports {reported_soc:.1f}% (SLEEP TRIGGERED!)")
else:
    print("  None detected in this dataset")

print("\nWith proposed calibration:")
new_false_triggers = [r for r in soc_results if r["below_cutoff_new"] and not r["below_cutoff_mm"]]
if new_false_triggers:
    for r in new_false_triggers:
        print(f"  Point {r['index']}: Still problematic")
else:
    print("  No false deep-sleep triggers! ✓")

# Calculate statistics
mean_old_error = sum(r["old_error_pct"] for r in soc_results) / len(soc_results)
mean_new_error = sum(r["new_error_pct"] for r in soc_results) / len(soc_results)
max_old_error = max(abs(r["old_error_pct"]) for r in soc_results)
max_new_error = max(abs(r["new_error_pct"]) for r in soc_results)

print(f"\n=== SoC Error Statistics ===")
print(f"Old Calibration:")
print(f"  Mean Error: {mean_old_error:+.1f}%")
print(f"  Max Error: {max_old_error:.1f}%")
print(f"\nNew Calibration:")
print(f"  Mean Error: {mean_new_error:+.1f}%")
print(f"  Max Error: {max_new_error:.1f}%")
print(f"\nImprovement:")
print(f"  Mean Error Reduction: {abs(mean_old_error) - abs(mean_new_error):.1f}%")
print(f"  Max Error Reduction: {max_old_error - max_new_error:.1f}%")

# Output JSON for HTML integration
output = {
    "soc_results": soc_results,
    "deep_sleep_threshold_v": deep_sleep_cutoff_v,
    "deep_sleep_threshold_soc_pct": voltage_to_soc_6v_sla(deep_sleep_cutoff_v),
    "statistics": {
        "old": {
            "mean_error_pct": mean_old_error,
            "max_error_pct": max_old_error
        },
        "new": {
            "mean_error_pct": mean_new_error,
            "max_error_pct": max_new_error
        }
    },
    "false_triggers_old": len(false_triggers),
    "false_triggers_new": len(new_false_triggers)
}

print("\n\n=== JSON FOR HTML ===")
print(json.dumps(output, indent=2))
