#!/usr/bin/env python3
"""Chaos rehearsal results (bench/chaos.sh): what the show did around each injected failure.

  chaos_report.py RAW_DIR > summary.json     prints a table to stderr

For every event: render fps and the output's new frames/s over the 10 s before it and over seconds 5-15 after it,
how long the per-second stats stopped (the engine was down), and the engine starts the supervisor counted.
"""
import json
import sys
from pathlib import Path


def lines(path):
    out = []
    if path.exists():
        for line in path.read_text().splitlines():
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def mean(values):
    return round(sum(values) / len(values), 2) if values else None


def window(stats, start, end):
    chunk = [s for s in stats if start <= s.get("unix", 0) < end]
    outputs = [o["new"] for s in chunk for o in s["outputs"] if o["attached"]]
    return {"fps": mean([s["render"]["fps"] for s in chunk]), "newFrames": mean(outputs),
            "videoMisses": sum(s["render"]["videoMisses"] for s in chunk), "seconds": len(chunk)}


def main():
    raw = Path(sys.argv[1])
    stats = [s for s in lines(raw / "show" / "stats.jsonl") if "unix" in s]
    events = lines(raw / "events.jsonl")
    rows = []
    for i, e in enumerate(events):
        at = e["unix"]
        until = events[i + 1]["unix"] if i + 1 < len(events) else at + 60
        times = [s["unix"] for s in stats if at - 1 <= s["unix"] <= until]
        gaps = [b - a for a, b in zip(times, times[1:])]
        rows.append({"event": e["event"], "detail": e["detail"], "before": window(stats, at - 10, at),
                     "after": window(stats, at + 5, min(at + 15, until)),
                     "statsStoppedS": round(max(gaps), 1) if gaps else None})
    notices = (raw / "notices.txt").read_text().splitlines() if (raw / "notices.txt").exists() else []
    summary = {"events": rows, "engineStarts": sum("engine started" in n for n in notices), "notices": notices}
    json.dump(summary, sys.stdout, indent=1)
    print()
    print(f"{'event':<16}{'fps before':>11}{'fps after':>10}{'new/s after':>12}{'stats gap s':>12}  detail",
          file=sys.stderr)
    for r in rows:
        print(f"{r['event']:<16}{r['before']['fps'] or '-':>11}{r['after']['fps'] or '-':>10}"
              f"{r['after']['newFrames'] or '-':>12}{r['statsStoppedS'] or '-':>12}  {r['detail']}", file=sys.stderr)
    print(f"engine starts: {summary['engineStarts']}", file=sys.stderr)
    for n in summary["notices"]:
        print(f"  {n}", file=sys.stderr)


if __name__ == "__main__":
    main()
