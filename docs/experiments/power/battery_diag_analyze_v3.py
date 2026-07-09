"""
TEMPORARY analysis script (v3) - combined median + sustained-drop-override.
The fast-phase capture revealed the "noise" is largely brief, one-directional
impulse dropouts (likely current-draw sag, e.g. WiFi TX bursts) rather than
symmetric white noise. This tests a median pre-filter (to reject those
impulses) feeding the sustained-drop-override EMA from v2.
"""
import csv
import statistics

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CSV_PATH = "battery_diag_raw.csv"
MV_PER_RAW_COUNT = 4.843

phases = {"fast": [], "medium": [], "slow": []}
with open(CSV_PATH, newline="") as f:
    reader = csv.DictReader(f)
    for row in reader:
        phases[row["phase"]].append({"elapsed_ms": int(row["elapsed_ms"]), "raw": int(row["raw"])})

medium = phases["medium"]
medium_t = [r["elapsed_ms"] for r in medium]
medium_raw = [r["raw"] for r in medium]


def simple_ema(raws, alpha):
    out, ema = [], None
    for r in raws:
        ema = float(r) if ema is None else ema + alpha * (r - ema)
        out.append(ema)
    return out


def median_prefilter(raws, n):
    out, buf = [], []
    for r in raws:
        buf.append(r)
        if len(buf) > n:
            buf.pop(0)
        out.append(statistics.median(buf))
    return out


def sustained_drop_override(times_ms, raws, alpha_slow, drop_threshold_raw, sustain_ms):
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
            ema = (sum(w_r for _, w_r in window) / len(window)) if sustained else ema + alpha_slow * (r - ema)
        out.append(ema)
    return out


DROP_THRESHOLD_RAW = 206
SUSTAIN_MS = 1000

# Combined: median-of-5 pre-filter feeding the sustained-drop-override EMA
median5 = median_prefilter(medium_raw, 5)
combined = sustained_drop_override(medium_t, median5, 0.02, DROP_THRESHOLD_RAW, SUSTAIN_MS)

candidates = {
    "current (EMA a=0.04)": simple_ema(medium_raw, 0.04),
    "sustained-drop override alone": sustained_drop_override(medium_t, medium_raw, 0.02, DROP_THRESHOLD_RAW, SUSTAIN_MS),
    "median(5) + sustained-drop override": combined,
}

print("=== Steady-state noise (medium phase) ===")
for name, series in candidates.items():
    stdev = statistics.pstdev(series)
    p2p = max(series) - min(series)
    print(f"{name:38s} stdev={stdev:6.3f} raw ({stdev*MV_PER_RAW_COUNT:5.1f} mV)   "
          f"p2p={p2p:6.2f} raw ({p2p*MV_PER_RAW_COUNT:5.1f} mV)")

# Synthetic drop test
last_t = medium_t[-1]
synth_t = medium_t + [last_t + (i + 1) * 50 for i in range(120)]
synth_raw = medium_raw + [5] * 120
drop_idx = len(medium_raw)
initial_val = medium_raw[-1]
threshold_10pct = initial_val * 0.10

synth_median5 = median_prefilter(synth_raw, 5)
drop_candidates = {
    "current (EMA a=0.04)": simple_ema(synth_raw, 0.04),
    "sustained-drop override alone": sustained_drop_override(synth_t, synth_raw, 0.02, DROP_THRESHOLD_RAW, SUSTAIN_MS),
    "median(5) + sustained-drop override": sustained_drop_override(synth_t, synth_median5, 0.02, DROP_THRESHOLD_RAW, SUSTAIN_MS),
}

print()
print(f"=== Synthetic power-loss reaction time (threshold={threshold_10pct:.0f}) ===")
for name, series in drop_candidates.items():
    t_reach = None
    for i in range(drop_idx, len(series)):
        if series[i] <= threshold_10pct:
            t_reach = (synth_t[i] - synth_t[drop_idx]) / 1000.0
            break
    print(f"{name:38s} {'%.2fs' % t_reach if t_reach is not None else 'not reached'}")

fig, ax = plt.subplots(figsize=(11, 6))
ax.plot([t/1000 for t in medium_t], medium_raw, label="raw (unfiltered)", color="lightgray", linewidth=0.7)
for (name, series), color in zip(candidates.items(), ["tab:blue", "tab:red", "tab:green"]):
    ax.plot([t/1000 for t in medium_t], series, label=name, color=color, linewidth=1.6)
ax.set_xlabel("seconds"); ax.set_ylabel("raw ADC counts")
ax.set_title("Normal operation: median pre-filter + sustained-drop override vs current")
ax.legend(loc="upper right", fontsize=8); ax.grid(True, alpha=0.3)
fig.tight_layout(); fig.savefig("battery_diag_v3_combined.png", dpi=130); plt.close(fig)
print("\nPlot written: battery_diag_v3_combined.png")
