"""Splits a demo run's per-second stats into the seconds after a trigger and the rest.

usage: trigger_seconds.py RUN_DIR   (stats.jsonl and triggers.txt from tools/demo-show.sh)

A trigger lands on its quantised beat, up to a bar after it is sent, so a trigger's window is the second it was sent
in and the next WINDOW seconds (default 3, a bar at 120 BPM plus the switch).
"""
import json
import os
import sys
from pathlib import Path


def per_second(run):
    stats = [json.loads(line) for line in open(run / "stats.jsonl") if line.strip()]
    marks = run / "triggers.txt"
    sent = [int(line.split()[0]) for line in open(marks)] if marks.exists() else []
    return stats, sent


def summarize(stats, sent, window):
    near = {s + k for s in sent for k in range(window + 1)}
    groups = {"trigger": [], "other": []}
    for i, s in enumerate(stats, 1):
        groups["trigger" if i in near else "other"].append(s)

    def measure(rows):
        if not rows:
            return None
        outputs = [o for r in rows for o in r["outputs"] if o["attached"]]
        render = [r["render"] for r in rows]
        return {
            "seconds": len(rows),
            "hitchesPerSPerOutput": round(sum(o["hitches"] for o in outputs) / max(1, len(outputs)), 3),
            "misses": sum(r["videoMisses"] for r in render),
            "skipped": sum(r["skipped"] for r in render),
            "gpuMaxMs": max(r["gpuMaxMs"] for r in render),
            "cpuMaxMs": max(r.get("cpuMaxMs", 0) for r in render),
            "doneMaxMs": max(r.get("doneMaxMs", 0) for r in render),
            "presentMaxMs": max((o.get("presentMaxMs", 0) for o in outputs), default=0),
            "decodeFpsMax": max(r["decode"]["fps"] for r in rows),
        }

    return {"triggers": len(sent), "window": window, **{k: measure(v) for k, v in groups.items()}}


if __name__ == "__main__":
    run = Path(sys.argv[1])
    stats, sent = per_second(run)
    print(json.dumps(summarize(stats, sent, int(os.environ.get("WINDOW", "3"))), indent=1))
