"""
TEMPORARY analysis script (v2) - refined candidate filters.
See battery_diag_analyze.py for the initial pass; this version fixes the
naive fast-attack/slow-decay candidate (which falsely triggered on normal
noise) by requiring a SUSTAINED drop over a time window, matching the
user's own suggested design: "greater than 1V change for more than 1
second -> snap immediately."
"""
import csv
import statistics

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CSV_PATH = "battery_diag_raw.csv"
MV_PER_RAW_COUNT = 52.93 / 10.93  # empirical, from medium-phase stdev ratio computed in pass 1

phases = {"fast": [], "medium": [], "slow": []}
with open(CSV_PATH, newline="") as f:
    reader = csv.DictReader(f)
    for row in reader:
        phases[row["phase"]].append({
            "elapsed_ms": int(row["elapsed_ms"]),
            "raw": int(row["raw"]),
        })

medium = phases["medium"]
medium_t = [r["elapsed_ms"] for r in medium]
medium_raw = [r["raw"] for r in medium]


def simple_ema(raws, alpha):
    out, ema = [], None
    for r in raws:
        ema = float(r) if ema is None else ema + alpha * (r - ema)
        out.append(ema)
    return out


def median_then_ema(raws, n, alpha):
    out, ema, buf = [], None, []
    for r in raws:
        buf.append(r)
        if len(buf) > n:
            buf.pop(0)
        med = statistics.median(buf)
        ema = med if ema is None else ema + alpha * (med - ema)
        out.append(ema)
    return out


def sustained_drop_override(times_ms, raws, alpha_slow, drop_threshold_raw, sustain_ms):
    """Normal operation: slow EMA (alpha_slow). Exception: if raw readings
    have stayed at least drop_threshold_raw BELOW the current filtered
    value for a continuous sustain_ms window, snap immediately to the
    recent window average instead of slowly creeping down."""
    out, ema, window = [], None, []
    for t, r in zip(times_ms, raws):
        window.append((t, r))
        while window and (t - window[0][0]) > sustain_ms:
            window.pop(0)
        if ema is None:
            ema = float(r)
        else:
            span = window[-1][0] - window[0][0]
            sustained = (span >= sustain_ms * 0.95 and
                         max(w_r for _, w_r in window) <= (ema - drop_threshold_raw))
            if sustained:
                ema = sum(w_r for _, w_r in window) / len(window)
            else:
                ema += alpha_slow * (r - ema)
        out.append(ema)
    return out


DROP_THRESHOLD_RAW = round(1000 / MV_PER_RAW_COUNT)  # ~1V in raw counts
SUSTAIN_MS = 1000

candidates = {
    "current (EMA a=0.04)": simple_ema(medium_raw, 0.04),
    "median(5)+EMA(a=0.06)": median_then_ema(medium_raw, 5, 0.06),
    "sustained-drop override (a=0.02, >=1V/1s)": sustained_drop_override(
        medium_t, medium_raw, 0.02, DROP_THRESHOLD_RAW, SUSTAIN_MS),
}

print(f"1V ~= {DROP_THRESHOLD_RAW} raw counts (using {MV_PER_RAW_COUNT:.3f} mV/count)")
print()
print("=== Steady-state noise (medium phase, normal operation) ===")
for name, series in candidates.items():
    stdev = statistics.pstdev(series)
    p2p = max(series) - min(series)
    print(f"{name:42s} stdev={stdev:6.3f} raw ({stdev*MV_PER_RAW_COUNT:5.1f} mV)   "
          f"p2p={p2p:6.2f} raw ({p2p*MV_PER_RAW_COUNT:5.1f} mV)")

# ---- Synthetic power-loss drop test ----
last_t = medium_t[-1]
synth_t = medium_t + [last_t + (i + 1) * 50 for i in range(120)]  # 50ms steps, 6s of near-zero
synth_raw = medium_raw + [5] * 120
drop_idx = len(medium_raw)
initial_val = medium_raw[-1]
threshold_10pct = initial_val * 0.10

drop_candidates = {
    "current (EMA a=0.04)": simple_ema(synth_raw, 0.04),
    "median(5)+EMA(a=0.06)": median_then_ema(synth_raw, 5, 0.06),
    "sustained-drop override (a=0.02, >=1V/1s)": sustained_drop_override(
        synth_t, synth_raw, 0.02, DROP_THRESHOLD_RAW, SUSTAIN_MS),
}

print()
print(f"=== Synthetic power-loss reaction time (initial={initial_val}, 10% threshold={threshold_10pct:.0f}) ===")
for name, series in drop_candidates.items():
    t_reach = None
    for i in range(drop_idx, len(series)):
        if series[i] <= threshold_10pct:
            t_reach = (synth_t[i] - synth_t[drop_idx]) / 1000.0
            break
    print(f"{name:42s} {'%.2fs' % t_reach if t_reach is not None else 'not reached in 6s window'}")

# ---- Plot: normal-operation comparison ----
fig, ax = plt.subplots(figsize=(11, 6))
ax.plot([t/1000 for t in medium_t], medium_raw, label="raw (unfiltered)", color="lightgray", linewidth=0.8)
for (name, series), color in zip(candidates.items(), ["tab:blue", "tab:purple", "tab:red"]):
    ax.plot([t/1000 for t in medium_t], series, label=name, color=color, linewidth=1.5)
ax.set_xlabel("seconds")
ax.set_ylabel("raw ADC counts")
ax.set_title("Normal operation (medium phase, 60s): refined candidates")
ax.legend(loc="upper right", fontsize=8)
ax.grid(True, alpha=0.3)
fig.tight_layout()
fig.savefig("battery_diag_v2_normal.png", dpi=130)
plt.close(fig)

# ---- Plot: power-loss reaction comparison ----
fig, ax = plt.subplots(figsize=(11, 6))
rel_t = [(t - synth_t[drop_idx]) / 1000.0 for t in synth_t[drop_idx - 40:]]
raw_win = synth_raw[drop_idx - 40:]
ax.plot(rel_t, raw_win, label="raw (with instant drop)", color="lightgray", linewidth=0.8)
for (name, series), color in zip(drop_candidates.items(), ["tab:blue", "tab:purple", "tab:red"]):
    ax.plot(rel_t, series[drop_idx - 40:], label=name, color=color, linewidth=1.8)
ax.axvline(0, color="black", linestyle="--", linewidth=1, label="power removed")
ax.axhline(threshold_10pct, color="gray", linestyle=":", linewidth=1, label="10% of pre-drop value")
ax.set_xlabel("seconds relative to power loss")
ax.set_ylabel("raw ADC counts")
ax.set_title("Simulated power-loss reaction time (refined candidates)")
ax.legend(loc="upper right", fontsize=8)
ax.grid(True, alpha=0.3)
fig.tight_layout()
fig.savefig("battery_diag_v2_dropoff.png", dpi=130)
plt.close(fig)

print()
print("Plots written: battery_diag_v2_normal.png, battery_diag_v2_dropoff.png")
