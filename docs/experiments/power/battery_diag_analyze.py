"""
TEMPORARY analysis script for the battery ADC diagnostic capture.
Reads battery_diag_raw.csv (phase,elapsed_ms,raw,voltage_mv) produced by the
temporary /api/battery/diag/* firmware routes, computes noise statistics per
phase, simulates several candidate filtering approaches against the captured
raw data, and produces comparison plots.

Not part of the shipped app; delete this script and the CSV once the
filtering decision has been made and applied.
"""
import csv
import statistics
import math

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

CSV_PATH = "battery_diag_raw.csv"

phases = {"fast": [], "medium": [], "slow": []}

with open(CSV_PATH, newline="") as f:
    reader = csv.DictReader(f)
    for row in reader:
        phase = row["phase"]
        phases[phase].append({
            "elapsed_ms": int(row["elapsed_ms"]),
            "raw": int(row["raw"]),
            "voltage_mv": int(row["voltage_mv"]),
        })

print("=== Sample counts ===")
for name, rows in phases.items():
    print(f"{name}: {len(rows)} samples, span {rows[-1]['elapsed_ms']/1000:.1f}s" if rows else f"{name}: 0 samples")

print()
print("=== Raw ADC noise stats per phase (unfiltered single reads) ===")
for name, rows in phases.items():
    raws = [r["raw"] for r in rows]
    mvs = [r["voltage_mv"] for r in rows]
    if not raws:
        continue
    mean_raw = statistics.mean(raws)
    stdev_raw = statistics.pstdev(raws)
    mean_mv = statistics.mean(mvs)
    stdev_mv = statistics.pstdev(mvs)
    p2p_raw = max(raws) - min(raws)
    p2p_mv = max(mvs) - min(mvs)
    print(f"[{name:6s}] n={len(raws):5d}  raw: mean={mean_raw:7.1f} stdev={stdev_raw:6.2f} p2p={p2p_raw:4d}   "
          f"mV: mean={mean_mv:7.1f} stdev={stdev_mv:6.2f} p2p={p2p_mv:4d}")

# ---- Candidate filter simulations, run against the "fast" phase (highest
# resolution, best for seeing true noise + filter behavior) and "medium"
# phase (to see behavior similar to real production timing). ----

def simple_ema(raws, alpha):
    out = []
    ema = None
    for r in raws:
        if ema is None:
            ema = float(r)
        else:
            ema += alpha * (r - ema)
        out.append(ema)
    return out

def moving_average(raws, window):
    out = []
    buf = []
    for r in raws:
        buf.append(r)
        if len(buf) > window:
            buf.pop(0)
        out.append(sum(buf) / len(buf))
    return out

def fast_attack_slow_decay_ema(raws, alpha_slow, alpha_fast, drop_threshold):
    """EMA that uses alpha_fast when the new raw sample is drop_threshold (raw
    counts) or more BELOW the current filtered value (a real nosedive), and
    alpha_slow otherwise (normal noise in either direction)."""
    out = []
    ema = None
    for r in raws:
        if ema is None:
            ema = float(r)
        else:
            if (ema - r) >= drop_threshold:
                ema += alpha_fast * (r - ema)
            else:
                ema += alpha_slow * (r - ema)
        out.append(ema)
    return out

def median_of_n_then_ema(raws, n, alpha):
    out = []
    buf = []
    ema = None
    for r in raws:
        buf.append(r)
        if len(buf) > n:
            buf.pop(0)
        med = statistics.median(buf)
        if ema is None:
            ema = med
        else:
            ema += alpha * (med - ema)
        out.append(ema)
    return out

medium_raws = [r["raw"] for r in phases["medium"]]
medium_ms = [r["elapsed_ms"] / 1000.0 for r in phases["medium"]]

candidates = {
    "current (a=0.04, 20Hz)": simple_ema(medium_raws, 0.04),
    "lower alpha (a=0.015)": simple_ema(medium_raws, 0.015),
    "moving avg (n=20, ~1s)": moving_average(medium_raws, 20),
    "fast-attack/slow-decay": fast_attack_slow_decay_ema(medium_raws, 0.03, 0.5, 40),
    "median(5)+EMA(a=0.06)": median_of_n_then_ema(medium_raws, 5, 0.06),
}

print()
print("=== Candidate filter output stats on 'medium' phase raw data ===")
for name, series in candidates.items():
    stdev = statistics.pstdev(series)
    p2p = max(series) - min(series)
    print(f"{name:28s} stdev={stdev:6.3f}  p2p={p2p:6.2f}")

# ---- Plot 1: raw noise floor from the fast phase ----
fast_ms = [r["elapsed_ms"] / 1000.0 for r in phases["fast"]]
fast_raw = [r["raw"] for r in phases["fast"]]
fast_mv = [r["voltage_mv"] for r in phases["fast"]]

fig, axes = plt.subplots(2, 1, figsize=(10, 7), sharex=True)
axes[0].plot(fast_ms, fast_raw, marker=".", markersize=2, linewidth=0.6, color="tab:blue")
axes[0].set_ylabel("raw ADC counts")
axes[0].set_title("Fast phase (~100Hz, 5s): raw ADC noise floor, unfiltered")
axes[0].grid(True, alpha=0.3)
axes[1].plot(fast_ms, fast_mv, marker=".", markersize=2, linewidth=0.6, color="tab:red")
axes[1].set_ylabel("voltage (mV)")
axes[1].set_xlabel("seconds")
axes[1].grid(True, alpha=0.3)
fig.tight_layout()
fig.savefig("battery_diag_fast_phase.png", dpi=130)
plt.close(fig)

# ---- Plot 2: medium phase raw vs candidate filters ----
fig, ax = plt.subplots(figsize=(11, 6))
ax.plot(medium_ms, medium_raws, label="raw (unfiltered)", color="lightgray", linewidth=0.8)
colors = ["tab:blue", "tab:orange", "tab:green", "tab:red", "tab:purple"]
for (name, series), color in zip(candidates.items(), colors):
    ax.plot(medium_ms, series, label=name, color=color, linewidth=1.4)
ax.set_xlabel("seconds")
ax.set_ylabel("raw ADC counts")
ax.set_title("Medium phase (~20Hz, 60s): raw vs candidate filters")
ax.legend(loc="upper right", fontsize=8)
ax.grid(True, alpha=0.3)
fig.tight_layout()
fig.savefig("battery_diag_medium_candidates.png", dpi=130)
plt.close(fig)

# ---- Plot 3: slow phase, raw + current filter (long-term view) ----
slow_ms = [r["elapsed_ms"] / 1000.0 for r in phases["slow"]]
slow_raw = [r["raw"] for r in phases["slow"]]
slow_current_ema = simple_ema(slow_raw, 0.04)
slow_lower_ema = simple_ema(slow_raw, 0.015)

fig, ax = plt.subplots(figsize=(11, 6))
ax.plot(slow_ms, slow_raw, label="raw (unfiltered)", color="lightgray", linewidth=0.8)
ax.plot(slow_ms, slow_current_ema, label="current EMA (a=0.04)", color="tab:blue", linewidth=1.4)
ax.plot(slow_ms, slow_lower_ema, label="lower EMA (a=0.015)", color="tab:green", linewidth=1.4)
ax.set_xlabel("seconds")
ax.set_ylabel("raw ADC counts")
ax.set_title("Slow phase (~2Hz, 300s): long-term drift/ripple")
ax.legend(loc="upper right", fontsize=9)
ax.grid(True, alpha=0.3)
fig.tight_layout()
fig.savefig("battery_diag_slow_phase.png", dpi=130)
plt.close(fig)

# ---- Simulate a power-loss step (concatenate a synthetic drop to ~0 raw
# counts onto the tail of the medium-phase data) to check reaction time of
# each candidate filter, per the user's <=5s requirement. ----
drop_point = medium_raws[-1]
synthetic = medium_raws + [5] * 100  # instantly drops to near-zero raw counts, held for 100 samples
synthetic_ms = medium_ms + [medium_ms[-1] + (i + 1) * 0.05 for i in range(100)]

drop_candidates = {
    "current (a=0.04)": simple_ema(synthetic, 0.04),
    "lower alpha (a=0.015)": simple_ema(synthetic, 0.015),
    "fast-attack/slow-decay": fast_attack_slow_decay_ema(synthetic, 0.03, 0.5, 40),
}

print()
print("=== Simulated power-loss step (medium-rate data + synthetic drop to ~0) ===")
print("Time to reach <10% of initial value after the drop:")
drop_idx = len(medium_raws)
threshold = drop_point * 0.10
for name, series in drop_candidates.items():
    t_reach = None
    for i in range(drop_idx, len(series)):
        if series[i] <= threshold:
            t_reach = synthetic_ms[i] - synthetic_ms[drop_idx]
            break
    print(f"{name:28s} {'%.2fs' % t_reach if t_reach is not None else 'not reached in window'}")

fig, ax = plt.subplots(figsize=(11, 6))
window = slice(drop_idx - 100, len(synthetic))
rel_ms = [synthetic_ms[i] - synthetic_ms[drop_idx] for i in range(drop_idx - 100, len(synthetic))]
ax.plot(rel_ms, synthetic[window], label="raw (with synthetic instant drop)", color="lightgray", linewidth=0.8)
for (name, series), color in zip(drop_candidates.items(), ["tab:blue", "tab:green", "tab:red"]):
    ax.plot(rel_ms, series[window], label=name, color=color, linewidth=1.6)
ax.axvline(0, color="black", linestyle="--", linewidth=1, label="power removed")
ax.axhline(threshold, color="gray", linestyle=":", linewidth=1, label="10% of pre-drop value")
ax.set_xlabel("seconds relative to power loss")
ax.set_ylabel("raw ADC counts")
ax.set_title("Simulated power-loss reaction time by filter")
ax.legend(loc="upper right", fontsize=8)
ax.grid(True, alpha=0.3)
fig.tight_layout()
fig.savefig("battery_diag_dropoff_reaction.png", dpi=130)
plt.close(fig)

print()
print("Plots written: battery_diag_fast_phase.png, battery_diag_medium_candidates.png, "
      "battery_diag_slow_phase.png, battery_diag_dropoff_reaction.png")
