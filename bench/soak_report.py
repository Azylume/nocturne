#!/usr/bin/env python3
"""Soak results (bench/soak.sh): per-minute series and a PASS/FAIL verdict as one JSON summary.

  soak_report.py RAW_DIR COMMIT [MINUTES] > summary.json   prints the verdict to stderr; exits 1 on FAIL
  MINUTES cuts the run to its first MINUTES minutes (to judge part of a longer run).

The soak is 30 minutes. After WARMUP_MIN minutes it is judged by slopes (least squares over the per-minute values,
extrapolated to an hour) and by 10-minute windows, so a leak or a cost that grows shows within the half hour:
- memory: physical footprint and RSS grow by less than MEMORY_SLOPE MB/h (slower leaks are out of scope), by a robust
  slope: the median of each minute's samples (taken every 10 s, so the frame caches' swings average out), then the
  Theil-Sen slope over those minutes; the decode cache is checked against its cap every second;
- render fps and the slowest output's new frames/s: every window at least FPS_MIN, and no slope below -FPS_SLOPE/h;
- video misses per minute: slope under MISS_SLOPE per hour;
- engine CPU and decode busy: slopes under CPU_SLOPE and BUSY_SLOPE points per hour;
- GPU ms per frame: slope under GPU_SLOPE of its mean per hour;
- temperature needs sudo, so its proxy, the CPU speed limit (pmset): every window within SPEED_DRIFT points of the first;
- no crash: one engine start, never safe mode.
Background CPU (mediaanalysisd, Spotlight) is reported with every window, to show whether it overlapped a failure.
"""
import json
import sys
from pathlib import Path

WARMUP_MIN = 5
WINDOW_MIN = 10
MEMORY_SLOPE = 10.0  # MB per hour
FPS_MIN = 59.0
FPS_SLOPE = 1.0  # fps per hour
MISS_SLOPE = 10.0  # misses per minute, per hour
CPU_SLOPE = 5.0  # engine CPU points per hour
BUSY_SLOPE = 5.0  # decode busy points per hour
GPU_SLOPE = 0.10  # of the mean GPU ms, per hour
SPEED_DRIFT = 5.0  # CPU speed limit points
BACKGROUND_BUSY = 10.0  # % CPU of mediaanalysisd that counts as busy


def lines(path):
    if not path.exists():
        return []
    out = []
    for line in path.read_text().splitlines():
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return out


def stats_lines(raw):
    rotated = sorted(raw.glob("stats.jsonl.*"), key=lambda p: -int(p.suffix[1:]))
    return [s for path in rotated + [raw / "stats.jsonl"] for s in lines(path)]


def mean(values):
    values = [v for v in values if v is not None]
    return round(sum(values) / len(values), 3) if values else None


def slope_per_hour(values):
    """Least-squares slope of per-minute values, in units per hour."""
    points = [(i, v) for i, v in enumerate(values) if v is not None]
    if len(points) < 2:
        return None
    n = len(points)
    mx = sum(i for i, _ in points) / n
    my = sum(v for _, v in points) / n
    sxx = sum((i - mx) ** 2 for i, _ in points)
    sxy = sum((i - mx) * (v - my) for i, v in points)
    return round(sxy / sxx * 60.0, 3) if sxx else None


def per_minute(stats):
    minutes = []
    for start in range(0, len(stats) - 59, 60):
        chunk = stats[start:start + 60]
        outputs = [o for s in chunk for o in s["outputs"] if o["attached"]]
        by_display = {}
        for o in outputs:
            by_display.setdefault(o["display"], []).append(o["new"])
        hitches_by_display = {}
        for o in outputs:
            hitches_by_display[o["display"]] = hitches_by_display.get(o["display"], 0) + o["hitches"]
        minutes.append({
            "fps": mean([s["render"]["fps"] for s in chunk]),
            "gpuMs": mean([s["render"]["gpuMs"] for s in chunk]),
            "gpuMaxMs": max(s["render"]["gpuMaxMs"] for s in chunk),
            "maxGapMs": max(s["render"].get("maxGapMs", 0.0) for s in chunk),
            "videoMisses": sum(s["render"]["videoMisses"] for s in chunk),
            "skipped": sum(s["render"]["skipped"] for s in chunk),
            "compiles": sum(s["render"]["compiles"] for s in chunk),
            "newFrames": min((mean(v) for v in by_display.values()), default=None),
            "outputs": {d: {"new": mean(v), "hitches": hitches_by_display.get(d, 0)} for d, v in by_display.items()},
            "dropped": sum(o.get("dropped", 0) for o in outputs),
            "hitches": sum(o["hitches"] for o in outputs),
            "decodeErrors": chunk[-1]["decode"]["errors"],
            "cacheMB": mean([s["decode"]["cacheMB"] for s in chunk]),
            "cpu": mean([s["cpu"] for s in chunk]),
        })
    return minutes


def theil_sen_per_hour(values):
    """The median of the slopes between every pair of per-minute values, in units per hour."""
    points = [(i, v) for i, v in enumerate(values) if v is not None]
    slopes = sorted((b - a) / (j - i) for k, (i, a) in enumerate(points) for (j, b) in points[k + 1:])
    if not slopes:
        return None
    mid = len(slopes) // 2
    return round((slopes[mid] if len(slopes) % 2 else (slopes[mid - 1] + slopes[mid]) / 2) * 60.0, 3)


def minute_medians(system, start):
    """The system samples grouped by minute since `start`, each field the median of its minute."""
    buckets = {}
    for sample in system:
        buckets.setdefault(int((sample["unix"] - start) // 60), []).append(sample)
    out = []
    for minute in range(max(buckets) + 1 if buckets else 0):
        group = buckets.get(minute, [])
        row = {}
        for key in ("footprintMB", "rssMB", "cpuSpeedLimit", "mediaAnalysisCpu", "spotlightCpu", "starts"):
            values = sorted(v for v in (g.get(key) for g in group) if v is not None)
            row[key] = values[len(values) // 2] if values else None
        row["mode"] = "safe" if any(g.get("mode") == "safe" for g in group) else "normal"
        out.append(row)
    return out


def windows(series, key):
    return [mean([m.get(key) for m in series[w:w + WINDOW_MIN]]) for w in range(0, len(series), WINDOW_MIN)
            if len(series[w:w + WINDOW_MIN]) >= WINDOW_MIN // 2]


def summarize(raw, commit, cut=None):
    stats = stats_lines(raw)
    samples = lines(raw / "minutes.jsonl")
    start = stats[0]["unix"] - stats[0]["t"] if stats and "unix" in stats[0] else (samples[0]["unix"] if samples else 0)
    system = minute_medians(samples, start)
    if cut:
        stats, system = stats[:cut * 60], system[:cut + 1]
    minutes = per_minute(stats)
    for m, sample in zip(minutes, system):
        m.update({k: sample.get(k) for k in ("footprintMB", "rssMB", "cpuSpeedLimit", "mediaAnalysisCpu", "spotlightCpu")})
    for m, chunk in zip(minutes, (stats[i:i + 60] for i in range(0, len(stats), 60))):
        m["decodeBusy"] = mean([x["decode"]["busy"] for x in chunk])
    measured = minutes[WARMUP_MIN:]
    measured_system = system[WARMUP_MIN:]
    checks = []

    def check(label, ok, value, rule):
        checks.append({"check": label, "pass": bool(ok), "value": value, "rule": rule})

    def slope(key, series=measured):
        return slope_per_hour([m.get(key) for m in series])

    over_cap = [round(x["decode"]["cacheMB"] - x["sizes"]["cacheCapMB"], 1) for x in stats
                if "sizes" in x and "cacheCapMB" in x["sizes"] and x["decode"]["cacheMB"] > x["sizes"]["cacheCapMB"]]
    caps = [x["sizes"]["cacheCapMB"] for x in stats if "sizes" in x and "cacheCapMB" in x["sizes"]]
    if caps:
        check("decode cache within its cap", not over_cap,
              {"secondsOver": len(over_cap), "worstOverMB": max(over_cap, default=0), "capMB": [min(caps), max(caps)],
               "cacheMB": [min(x["decode"]["cacheMB"] for x in stats), max(x["decode"]["cacheMB"] for x in stats)]},
              "cache <= cap every second (the clip cache bound per playing clip plus the preload budget)")
    for key in ("footprintMB", "rssMB"):
        value = theil_sen_per_hour([m.get(key) for m in measured_system])
        check(f"{key} slope", value is not None and value < MEMORY_SLOPE,
              {"MBPerHour": value, "windows": windows(measured_system, key)}, f"< {MEMORY_SLOPE} MB/h")
    win, value = windows(measured, "fps"), slope("fps")
    check("render fps", win and min(win) >= FPS_MIN and value is not None and value > -FPS_SLOPE,
          {"windows": win, "slopePerHour": value}, f"every window >= {FPS_MIN}, slope > -{FPS_SLOPE}/h")
    # Virtual windows share the one display and WindowServer, so their absolute rates say little: trend only.
    displays = sorted({d for m in measured for d in m["outputs"]})
    for d in displays:
        virtual = d.startswith("window-")
        series = [{"new": m["outputs"].get(d, {}).get("new"), "hitches": m["outputs"].get(d, {}).get("hitches")}
                  for m in measured]
        new_win, new_slope = windows(series, "new"), slope_per_hour([m["new"] for m in series])
        floor = virtual or (new_win and min(new_win) >= FPS_MIN)
        check(f"output {d} new frames/s", floor and new_slope is not None and new_slope > -FPS_SLOPE,
              {"windows": new_win, "slopePerHour": new_slope, "virtual": virtual},
              f"slope > -{FPS_SLOPE}/h" + ("" if virtual else f", every window >= {FPS_MIN}"))
        hitch_win, hitch_slope = windows(series, "hitches"), slope_per_hour([m["hitches"] for m in series])
        check(f"output {d} hitches/min slope", hitch_slope is not None and hitch_slope < MISS_SLOPE,
              {"perMinuteByWindow": hitch_win, "slopePerHour": hitch_slope}, f"< {MISS_SLOPE}/h")
    for key, label, limit in (("videoMisses", "misses per minute", MISS_SLOPE), ("cpu", "engine CPU %", CPU_SLOPE),
                              ("decodeBusy", "decode busy %", BUSY_SLOPE)):
        value = slope(key)
        check(f"{label} slope", value is not None and value < limit,
              {"slopePerHour": value, "windows": windows(measured, key)}, f"< {limit}/h")
    gpu, gpu_mean = slope("gpuMs"), mean([m["gpuMs"] for m in measured])
    gpu_rel = round(gpu / gpu_mean, 4) if gpu is not None and gpu_mean else None
    check("GPU ms slope", gpu_rel is not None and gpu_rel < GPU_SLOPE,
          {"slopePerHour": gpu, "relative": gpu_rel, "windows": windows(measured, "gpuMs")},
          f"< {GPU_SLOPE:.0%} of the mean per hour")
    speed = windows(measured_system, "cpuSpeedLimit")
    speed_drift = round(max(abs(h - speed[0]) for h in speed), 2) if speed and None not in speed else None
    check("CPU speed limit", speed_drift is not None and speed_drift <= SPEED_DRIFT,
          {"windows": speed, "drift": speed_drift}, f"every window within {SPEED_DRIFT} points of the first")
    starts = max((s.get("starts") or 0 for s in system), default=0)
    safe = any(s.get("mode") == "safe" for s in system)
    check("crashes", starts == 1 and not safe, {"engineStarts": starts, "safeMode": safe},
          "one engine start, never safe mode")

    analysis = [s.get("mediaAnalysisCpu") or 0.0 for s in system]
    return {
        "commit": commit,
        "minutes": len(minutes),
        "warmupMinutes": WARMUP_MIN,
        "windowMinutes": WINDOW_MIN,
        "pass": all(c["pass"] for c in checks),
        "checks": checks,
        "cacheOverPlanMB": max((round(x["decode"]["cacheMB"] - x["sizes"]["cachePlanMB"]) for x in stats
                                if "cachePlanMB" in x.get("sizes", {})), default=None),
        "windows": {key: windows(measured, key) for key in
                    ("videoMisses", "dropped", "hitches", "maxGapMs", "cacheMB", "mediaAnalysisCpu", "spotlightCpu")},
        "totals": {key: sum(m[key] for m in minutes) for key in
                   ("videoMisses", "skipped", "dropped", "hitches", "compiles")},
        "decodeErrors": minutes[-1]["decodeErrors"] if minutes else None,
        "longestRenderGapMs": max((m["maxGapMs"] for m in minutes), default=None),
        "mediaAnalysisMaxCpu": max(analysis, default=None),
        "series": {"minutes": minutes, "system": system},
    }


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    summary = summarize(Path(sys.argv[1]), sys.argv[2], int(sys.argv[3]) if len(sys.argv) == 4 else None)
    json.dump(summary, sys.stdout, indent=1)
    print(file=sys.stdout)
    print(f"soak {summary['minutes']} min: {'PASS' if summary['pass'] else 'FAIL'}", file=sys.stderr)
    for c in summary["checks"]:
        print(f"  {'PASS' if c['pass'] else 'FAIL'}  {c['check']:<28} {json.dumps(c['value'])}  ({c['rule']})",
              file=sys.stderr)
    w = summary["windows"]
    print(f"  info  per 10 min: misses {w['videoMisses']}, dropped {w['dropped']}, mediaanalysisd {w['mediaAnalysisCpu']}%;"
          f" totals {summary['totals']}; decode errors {summary['decodeErrors']}; longest render gap "
          f"{summary['longestRenderGapMs']} ms", file=sys.stderr)
    sys.exit(0 if summary["pass"] else 1)


if __name__ == "__main__":
    main()
