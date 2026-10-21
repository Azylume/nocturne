#!/usr/bin/env python3
"""Compares soaks run with PREVIEW_CLIP=1 (bench/soak.sh), clip previews off and on.

usage: bench/preview_soak_compare.py OFF_RAW_DIR ON_RAW_DIR [--from-minute 5]

For each run, from minute 5 on: render fps, GPU ms per frame, video misses, skipped renders, output hitches per
minute, the engine's footprint and RSS (median of the first and last five minutes), and around each preview clip edit
(edits.log) the longest render gap and the show's compiles in the 10 s after it against the rest. The on run also has
the preview's pictures drawn with effects per second, their GPU ms and the preview's own compiles. The preview and clip
sizes of the stats (and mallocMB) are given as [first minute median, last minute median, max]."""

import json
import statistics
import sys
from pathlib import Path


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


def mean(values):
    values = [v for v in values if v is not None]
    return round(sum(values) / len(values), 3) if values else None


def summarize(raw, start_minute):
    stats = lines(raw / "stats.jsonl")
    if not stats:
        return {}
    t0 = stats[0]["unix"]
    measured = [s for s in stats if s["unix"] - t0 >= start_minute * 60]
    minutes = (measured[-1]["unix"] - measured[0]["unix"]) / 60 if len(measured) > 1 else 0
    edits = []
    if (raw / "edits.log").exists():
        edits = [int(line.split()[0]) for line in (raw / "edits.log").read_text().split("\n") if line.strip()]
    near = lambda s: any(0 <= s["unix"] - e < 10 for e in edits)
    after = [s for s in measured if near(s)]
    rest = [s for s in measured if not near(s)]
    hitches = sum(o["hitches"] for s in measured for o in s.get("outputs", []))
    system = [m for m in lines(raw / "minutes.jsonl") if m.get("unix", 0) - t0 >= start_minute * 60]
    first = [m for m in system if m["unix"] - system[0]["unix"] < 300] if system else []
    last = [m for m in system if system[-1]["unix"] - m["unix"] < 300] if system else []
    med = lambda rows, key: statistics.median([r[key] for r in rows if r.get(key) is not None]) if rows else None
    summary = {
        "minutes": round(minutes, 1),
        "renderFps": mean([s["render"]["fps"] for s in measured]),
        "gpuMs": mean([s["render"]["gpuMs"] for s in measured]),
        "gpuMaxMs": max(s["render"]["gpuMaxMs"] for s in measured),
        "videoMisses": sum(s["render"]["videoMisses"] for s in measured),
        "skipped": sum(s["render"]["skipped"] for s in measured),
        "hitchesPerMinute": round(hitches / minutes, 1) if minutes else None,
        "edits": len([e for e in edits if e - t0 >= start_minute * 60]),
        "maxGapMsAfterEdits": max((s["render"].get("maxGapMs", 0) for s in after), default=None),
        "maxGapMsElsewhere": max((s["render"].get("maxGapMs", 0) for s in rest), default=None),
        "skippedAfterEdits": sum(s["render"]["skipped"] for s in after),
        "showCompileMaxMsAfterEdits": max((s["render"]["compileMaxMs"] for s in after), default=None),
        "footprintMB": [med(first, "footprintMB"), med(last, "footprintMB")],
        "rssMB": [med(first, "rssMB"), med(last, "rssMB")],
        "engineCpu": mean([m.get("engineCpu") for m in system]),
    }
    sized = [s["sizes"] for s in measured if "sizes" in s]
    names = sorted({k for z in sized for k in z if k.startswith(("preview", "clip")) or k == "mallocMB"})
    if names:
        start_sizes = [z for z, s in zip(sized, measured) if s["unix"] - measured[0]["unix"] < 60]
        end_sizes = [z for z, s in zip(sized, measured) if measured[-1]["unix"] - s["unix"] < 60]
        summary["sizes"] = {k: [med(start_sizes, k), med(end_sizes, k), max(z.get(k, 0) for z in sized)]
                            for k in names}
    previews = [s["clipPreviews"] for s in measured if "clipPreviews" in s]
    if previews and any("effectFps" in p for p in previews):
        drawn = [p for p in previews if p.get("effectFps")]
        summary["preview"] = {
            "picturesPerSecond": mean([p["fps"] for p in previews]),
            "effectPicturesPerSecond": mean([p.get("effectFps", 0) for p in previews]),
            "effectGpuMs": mean([p["effectGpuMs"] for p in drawn]),
            "compiles": sum(p.get("compiles", 0) for p in previews),
            "compileMaxMs": max((p.get("compileMaxMs", 0) for p in previews), default=None),
            "samplingBusyPercent": mean([p["busy"] for p in previews]),
        }
    return summary


def main():
    args = sys.argv[1:]
    start = 5
    if "--from-minute" in args:
        i = args.index("--from-minute")
        start = float(args[i + 1])
        del args[i : i + 2]
    if len(args) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    print(json.dumps({"off": summarize(Path(args[0]), start), "on": summarize(Path(args[1]), start)}, indent=2))


if __name__ == "__main__":
    main()
