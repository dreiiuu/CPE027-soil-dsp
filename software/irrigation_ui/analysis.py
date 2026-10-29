"""Dashboard-side signal analysis. DISPLAY ONLY: nothing here feeds back into pump control,
which stays on the ESP32 (handoff section 8.4 / 8.8).

All functions take plain lists so the app needs nothing beyond the standard library.
Time series are sampled by the firmware at ~1 Hz; `ms` is device uptime.
"""
from __future__ import annotations

import statistics
from typing import Optional, Sequence


# ------------------------------------------------------------------ filters (for comparison plots)
def moving_average(x: Sequence[float], m: int = 10) -> list:
    out, s, q = [], 0.0, []
    for v in x:
        q.append(v)
        s += v
        if len(q) > m:
            s -= q.pop(0)
        out.append(s / len(q))
    return out


def ema(x: Sequence[float], alpha: float = 0.25) -> list:
    """Same recursion as the firmware (seeded with the first sample)."""
    out = []
    for v in x:
        out.append(v if not out else out[-1] + alpha * (v - out[-1]))
    return out


def running_median(x: Sequence[float], n: int = 5) -> list:
    out = []
    for i in range(len(x)):
        w = sorted(x[max(0, i - n + 1): i + 1])
        out.append(w[len(w) // 2])
    return out


def median_then_ema(x: Sequence[float], n: int = 5, alpha: float = 0.1) -> list:
    """Two-stage filter from the CPE027 report: 5-point running median, then EMA."""
    return ema(running_median(x, n), alpha)


FILTERS = {
    "ma10": ("Moving average, M=10", lambda x: moving_average(x, 10)),
    "ema25": ("EMA alpha=0.25 (same as firmware)", lambda x: ema(x, 0.25)),
    "ema10": ("EMA alpha=0.10", lambda x: ema(x, 0.10)),
    "med5ema": ("Running median N=5 + EMA alpha=0.10", lambda x: median_then_ema(x, 5, 0.10)),
}


# ------------------------------------------------------------------ spikes
def hampel_spikes(x: Sequence[float], half: int = 3, k: float = 3.0, floor: float = 8.0) -> list:
    """Indices where |x - centred median| > k * 1.4826 * MAD (MAD floored at `floor` counts so a
    very quiet signal does not flag ADC LSB noise)."""
    idx = []
    for i in range(half, len(x) - half):
        w = x[i - half: i + half + 1]
        med = statistics.median(w)
        mad = statistics.median(abs(v - med) for v in w)
        if abs(x[i] - med) > k * max(1.4826 * mad, floor):
            idx.append(i)
    return idx


def noise_sd(x: Sequence[float]) -> Optional[float]:
    """Sample-to-sample noise, robust to slow trends and to single steps (a pump pulse):
    1.4826 * MAD(first differences) / sqrt(2). Equals the SD for white noise on a constant."""
    if len(x) < 10:
        return None
    d = [b - a for a, b in zip(x, x[1:])]
    med = statistics.median(d)
    return 1.4826 * statistics.median(abs(v - med) for v in d) / 2 ** 0.5


# ------------------------------------------------------------------ trends
def slope_per_hour(t_s: Sequence[float], y: Sequence[float]) -> Optional[float]:
    """Least-squares slope in y-units per hour, or None with fewer than 30 points."""
    pts = [(t, v) for t, v in zip(t_s, y) if v is not None]
    if len(pts) < 30:
        return None
    n = len(pts)
    mt = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    sxx = sum((p[0] - mt) ** 2 for p in pts)
    if sxx == 0:
        return None
    sxy = sum((p[0] - mt) * (p[1] - my) for p in pts)
    return sxy / sxx * 3600.0


def drying_rate(t_s, avg_pct, pump, window_s: float = 600.0) -> Optional[float]:
    """Slope of average % over the last `window_s`, only from samples after the last pump pulse
    (+60 s settling) so watering does not count as negative drying. %/h, negative = drying."""
    if not t_s:
        return None
    last_on = max((t for t, p in zip(t_s, pump) if p), default=None)
    start = t_s[-1] - window_s
    if last_on is not None:
        start = max(start, last_on + 60.0)
    sel = [(t, v) for t, v in zip(t_s, avg_pct) if t >= start and v is not None]
    return slope_per_hour([s[0] for s in sel], [s[1] for s in sel])


def pulse_responses(t_s, avg_pct, pump, rise: float = 2.0, max_wait_s: float = 60.0) -> list:
    """For every pump-command rising edge: seconds until average % first rises `rise` points above
    its value at the pulse, or None if it did not within `max_wait_s`. This is the lag of the
    *sensor reading*, not measured water flow (there is no flow sensor)."""
    out = []
    for i in range(1, len(t_s)):
        if pump[i] and not pump[i - 1] and avg_pct[i - 1] is not None:
            base, t0, lag = avg_pct[i - 1], t_s[i], None
            for j in range(i, len(t_s)):
                if t_s[j] - t0 > max_wait_s:
                    break
                if avg_pct[j] is not None and avg_pct[j] >= base + rise:
                    lag = t_s[j] - t0
                    break
            out.append({"t": t0, "base_pct": base, "lag_s": lag})
    return out


# ------------------------------------------------------------------ health / calibration quality
def sensor_report(raw: Sequence[Sequence[int]], pct: Sequence[Sequence[Optional[int]]], t_s, pump) -> dict:
    """raw / pct are per-sample triples. Uses the last 60 samples for noise, the last 10 min for drift."""
    rep = {"noise_sd": [None] * 3, "spikes_recent": [0] * 3, "drift_counts_per_h": [None] * 3,
           "spread_pct": None, "pinned": [False] * 3, "warnings": []}
    if not raw:
        return rep
    n = len(raw)
    for i in range(3):
        col = [r[i] for r in raw]
        sd = noise_sd(col[-60:])
        rep["noise_sd"][i] = None if sd is None else round(sd, 1)
        rep["spikes_recent"][i] = len(hampel_spikes(col[-300:]))
        # drift: raw slope over the last 10 min, only if the pump was never commanded in that window
        sel = [(t, c) for t, c, p in zip(t_s, col, pump) if t >= t_s[-1] - 600]
        if sel and not any(p for t, p in zip(t_s, pump) if t >= t_s[-1] - 600):
            s = slope_per_hour([a for a, _ in sel], [b for _, b in sel])
            rep["drift_counts_per_h"][i] = None if s is None else round(s, 1)
        recent = [p[i] for p in pct[-120:] if p[i] is not None]
        rep["pinned"][i] = len(recent) >= 120 and (all(v == 0 for v in recent) or all(v == 100 for v in recent))
    last = pct[-1]
    if all(v is not None for v in last):
        rep["spread_pct"] = max(last) - min(last)
        recent = [p for p in pct[-30:] if all(v is not None for v in p)]
        if len(recent) >= 30 and all(max(p) - min(p) > 25 for p in recent):
            rep["warnings"].append("The three probes disagree by more than 25 points for 30 s: check insertion depth, "
                                   "placement, or redo the D/W calibration.")
    for i in range(3):
        if rep["pinned"][i]:
            rep["warnings"].append(f"S{i + 1} has read exactly 0% or 100% for 2 min: it is outside its dry/wet "
                                   "references. Recalibrate, or the probe is not in the soil.")
        sd = rep["noise_sd"][i]
        if sd is not None and sd > 40:
            rep["warnings"].append(f"S{i + 1} is noisy (about {sd} counts sample to sample): check the AO wire, ground and "
                                   "routing away from the pump wires.")
    drifts = [d for d in rep["drift_counts_per_h"] if d is not None]
    if len(drifts) == 3:
        med = statistics.median(drifts)
        for i, d in enumerate(rep["drift_counts_per_h"]):
            if abs(d - med) > 150:
                rep["warnings"].append(f"S{i + 1} drifts differently from the other two ({d:+.0f} vs {med:+.0f} "
                                       "counts/h with the pump off): possible probe or connection problem.")
    return rep
