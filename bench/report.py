#!/usr/bin/env python3
"""Benchmark results: summarise raw samples into one compact result, and judge a result against the gate.

  report.py summarize RAW_DIR COMMIT DIRTY > result.json
  report.py compare RESULT [BASELINE]     prints a PASS/FAIL table; exits 1 on FAIL
"""
import json
import math
import sys
from datetime import datetime, timezone
from pathlib import Path

# The typical composition must hold 60 fps on every output; GPU time may not grow past the baseline's by more than this.
FPS_MIN = 59.5
RATE_MAX = 0.05  # skipped renders, video misses and hitches per output, per second
GPU_TOLERANCE = 0.10
# Only typical is gated; heavy is judged by the same rules for information.
SHOWS = ("typical", "heavy")


def lines(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()] if path.exists() else []


def avg(values):
    return round(sum(values) / len(values), 2) if values else None


def p95(values):
    if not values:
        return None
    ordered = sorted(values)
    return round(ordered[min(len(ordered) - 1, math.ceil(0.95 * len(ordered)) - 1)], 2)


def peak(values):
    return round(max(values), 2) if values else None


def per_second(total, seconds):
    return round(total / seconds, 3) if seconds else None


def summarize_outputs(stats, seconds):
    displays = {}
    for s in stats:
        for o in s["outputs"]:
            if o["attached"]:
                displays.setdefault(o["display"], []).append(o)
    return {
        display: {
            "hz": round(samples[-1]["hz"], 2),
            "presents": avg([o["presents"] for o in samples]),
            "shown": avg([o["shown"] for o in samples]),
            "newAvg": avg([o["new"] for o in samples]),
            "newMin": round(min(o["new"] for o in samples), 2),
            "hitches": sum(o["hitches"] for o in samples),
            "hitchesPerS": per_second(sum(o["hitches"] for o in samples), seconds),
            "maxIntervalMs": peak([o["maxIntervalMs"] for o in samples]),
        }
        for display, samples in displays.items()
    }


def misses_by_layer(render):
    totals = {}
    for r in render:
        for layer, count in r.get("missesByLayer", {}).items():
            totals[layer] = totals.get(layer, 0) + count
    return dict(sorted(totals.items(), key=lambda item: int(item[0])))


def summarize_run(run_dir):
    stats = lines(run_dir / "stats.jsonl")
    system = lines(run_dir / "sys.jsonl")
    seconds = len(stats)
    render = [s["render"] for s in stats]
    decode = [s["decode"] for s in stats]
    frames = sum(r["fps"] for r in render)
    skipped = sum(r["skipped"] for r in render)
    misses = sum(r["videoMisses"] for r in render)

    def sampled(key):
        return [s[key] for s in system if s.get(key) is not None]

    return {
        "seconds": seconds,
        "render": {
            "fpsAvg": avg([r["fps"] for r in render]),
            "fpsMin": round(min(r["fps"] for r in render), 2) if render else None,
            "gpuMsAvg": round(sum(r["gpuMs"] * r["fps"] for r in render) / frames, 2) if frames else None,
            "gpuMsP95": p95([r["gpuMs"] for r in render]),
            "gpuMsMax": peak([r["gpuMaxMs"] for r in render]),
            "skipped": skipped,
            "skippedPerS": per_second(skipped, seconds),
            "videoMisses": misses,
            "videoMissesPerS": per_second(misses, seconds),
            "videoMissesByLayer": misses_by_layer(render),
            "passes": render[-1]["passes"] if render else None,
            "intermediates": render[-1]["intermediates"] if render else None,
            "compiles": sum(r["compiles"] for r in render),
            "cpuAvg": avg([s["cpu"] for s in stats]),
        },
        "decode": {
            "fpsAvg": avg([d["fps"] for d in decode]),
            "busyAvg": avg([d["busy"] for d in decode]),
            "busyMax": peak([d["busy"] for d in decode]),
            "seeks": sum(d["seeks"] for d in decode),
            "errors": decode[-1]["errors"] if decode else None,
            "stalls": decode[-1].get("stalls") if decode else None,
            "readMaxMs": peak([d["readMaxMs"] for d in decode if "readMaxMs" in d]),
        },
        "outputs": summarize_outputs(stats, seconds),
        "system": {
            "gpuUtilAvg": avg(sampled("gpuUtil")),
            "gpuUtilMax": peak(sampled("gpuUtil")),
            "windowServerCpuAvg": avg(sampled("windowServerCpu")),
            "windowServerCpuMax": peak(sampled("windowServerCpu")),
            "engineCpuAvg": avg(sampled("engineCpu")),
            "cpuSpeedLimitMin": min(sampled("cpuSpeedLimit"), default=None),
            "mediaAnalysisCpuAvg": avg(sampled("mediaAnalysisCpu")),
            "mediaAnalysisCpuMax": peak(sampled("mediaAnalysisCpu")),
            "spotlightCpuAvg": avg(sampled("spotlightCpu")),
            "spotlightCpuMax": peak(sampled("spotlightCpu")),
        },
        "record": summarize_record(stats, seconds),
    }


def summarize_record(stats, seconds):
    record = [s["record"] for s in stats if "record" in s]
    if not any(r["recording"] for r in record):
        return None
    dropped = sum(r["dropped"] for r in record)
    return {
        "codec": record[-1]["codec"],
        "hardware": record[-1]["hardware"],
        "secondsRecording": sum(1 for r in record if r["recording"]),
        "fpsAvg": avg([r["fps"] for r in record]),
        "fpsMin": round(min(r["fps"] for r in record), 2),
        "dropped": dropped,
        "droppedPerS": per_second(dropped, seconds),
    }


def holds_60(run):
    outputs = run["outputs"].values()
    return (run["render"]["fpsAvg"] >= FPS_MIN and len(outputs) > 0
            and all(o["newAvg"] >= FPS_MIN for o in outputs))


def summarize(raw, commit, dirty):
    result = {
        "schema": 1,
        "date": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "commit": commit,
        "dirty": dirty == "1",
        "machine": json.loads((raw / "machine.json").read_text()),
    }
    for name in SHOWS:
        if (raw / name).is_dir():
            result[name] = summarize_run(raw / name)
    stress = sorted((raw / "stress").glob("*x*"), key=lambda d: [int(n) for n in d.name.split("x")])
    if stress:
        levels = [{"level": d.name, **summarize_run(d)} for d in stress]
        broken = [lv["level"] for lv in levels if not holds_60(lv)]
        result["stress"] = {"breaksAt": broken[0] if broken else None, "levels": levels}
    json.dump(result, sys.stdout, indent=1)
    print()


def checks(result, name, baseline):
    run = result[name]
    expected = len(result["machine"]["displays"].split())
    r = run["render"]
    rows = [
        ("render fps avg", r["fpsAvg"], f">= {FPS_MIN}", r["fpsAvg"] >= FPS_MIN),
        ("skipped renders /s", r["skippedPerS"], f"<= {RATE_MAX}", r["skippedPerS"] <= RATE_MAX),
        ("video misses /s", r["videoMissesPerS"], f"<= {RATE_MAX}", r["videoMissesPerS"] <= RATE_MAX),
        ("pipelines compiled while measuring", r["compiles"], "== 0", r["compiles"] == 0),
        ("decode errors", run["decode"]["errors"], "== 0", run["decode"]["errors"] == 0),
        ("outputs", len(run["outputs"]), f"== {expected}", len(run["outputs"]) == expected),
    ]
    for display, o in sorted(run["outputs"].items()):
        rows.append((f"{display} new frames/s avg", o["newAvg"], f">= {FPS_MIN}", o["newAvg"] >= FPS_MIN))
        rows.append((f"{display} hitches /s", o["hitchesPerS"], f"<= {RATE_MAX}", o["hitchesPerS"] <= RATE_MAX))
    if baseline and name in baseline:
        limit = round(baseline[name]["render"]["gpuMsAvg"] * (1 + GPU_TOLERANCE), 2)
        rows.append(("gpu ms avg", r["gpuMsAvg"], f"<= {limit} (baseline {baseline['commit']} +10%)",
                     r["gpuMsAvg"] <= limit))
    else:
        rows.append(("gpu ms avg", r["gpuMsAvg"], "no baseline, not compared", True))
    return rows


def print_stress(result, baseline):
    stress = result.get("stress")
    if not stress:
        return
    before = (baseline or {}).get("stress", {}).get("breaksAt", "-")
    print(f"\nstress ramp (informational), 60 fps breaks at {stress['breaksAt'] or 'none'} (baseline: {before})")
    print(f"  {'level':<7}{'fps':>7}{'gpu ms':>8}{'p95':>7}{'new/s min':>11}{'misses/s':>10}{'decode %':>10}"
          f"{'gpu %':>7}{'cpu %':>7}")
    for lv in stress["levels"]:
        r = lv["render"]
        new_min = min((o["newAvg"] for o in lv["outputs"].values()), default=0)
        print(f"  {lv['level']:<7}{r['fpsAvg']:>7}{r['gpuMsAvg']:>8}{r['gpuMsP95']:>7}{new_min:>11}"
              f"{r['videoMissesPerS']:>10}{lv['decode']['busyAvg']:>10}{lv['system']['gpuUtilAvg'] or '-':>7}"
              f"{r['cpuAvg']:>7}")


def print_show(result, name, baseline):
    rows = checks(result, name, baseline)
    passed = all(ok for *_, ok in rows)
    width = max(len(label) for label, *_ in rows)
    gated = "gated" if name == "typical" else "informational"
    print(f"\n{name} ({result[name]['seconds']} s measured, {gated}): {'PASS' if passed else 'FAIL'}")
    for label, value, rule, ok in rows:
        print(f"  {'PASS' if ok else 'FAIL'}  {label:<{width}}  {value!s:>8}  {rule}")
    run = result[name]
    r, s = run["render"], run["system"]
    print(f"  info  gpu ms p95 {r['gpuMsP95']}, max {r['gpuMsMax']}; cpu {r['cpuAvg']}%; "
          f"decode busy {run['decode']['busyAvg']}% (max {run['decode']['busyMax']}%); GPU util {s['gpuUtilAvg']}%; "
          f"WindowServer {s['windowServerCpuAvg']}%; CPU speed limit min {s['cpuSpeedLimitMin']}; "
          f"mediaanalysisd max {s.get('mediaAnalysisCpuMax')}%, Spotlight max {s.get('spotlightCpuMax')}%")
    print(f"  info  video misses by layer {r.get('videoMissesByLayer', {})}")
    if run.get("record"):
        rec = run["record"]
        engine = {True: "hardware", False: "software"}.get(rec["hardware"], "encoder unknown")
        print(f"  info  recording {rec['codec']} ({engine}) for {rec['secondsRecording']} of {run['seconds']} s: "
              f"{rec['fpsAvg']} fps (min {rec['fpsMin']}), {rec['dropped']} dropped ({rec['droppedPerS']}/s)")
    return passed


def compare(result_path, baseline_path=None):
    result = json.loads(Path(result_path).read_text())
    baseline = json.loads(Path(baseline_path).read_text()) if baseline_path else None
    print(f"benchmark {result['commit']}{' (dirty)' if result['dirty'] else ''} on {result['machine']['model']}, "
          f"macOS {result['machine']['macos']}, outputs {result['machine']['displays']}"
          + (f", recording {result['machine']['record']}" if result["machine"].get("record") else ""))
    if "typical" not in result:
        print("FAIL: no typical run")
        return 1
    passed = print_show(result, "typical", baseline)
    if "heavy" in result:
        print_show(result, "heavy", baseline)
    print_stress(result, baseline)
    print(f"\nverdict (typical): {'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "summarize":
        summarize(Path(sys.argv[2]), sys.argv[3], sys.argv[4])
    elif len(sys.argv) in (3, 4) and sys.argv[1] == "compare":
        sys.exit(compare(*sys.argv[2:]))
    else:
        sys.exit(__doc__)
