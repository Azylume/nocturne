#!/usr/bin/env python3
"""Judges a watched show (tools/watched-show.sh): for each event, the render rate before and after it, the seconds
until the render is back at 59 fps, and the video misses, skipped renders and output hitches in the minute after it;
over the whole session, render fps, misses and the recording's frames.

usage: bench/watched_report.py STATS.jsonl EVENTS.jsonl"""

import json
import statistics
import sys


def lines(path):
    out = []
    with open(path) as f:
        for line in f:
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def window(stats, start, end):
    return [s for s in stats if start <= s["unix"] < end]


def summary(chunk):
    if not chunk:
        return None
    outputs = [o for s in chunk for o in s.get("outputs", []) if o.get("attached")]
    return {
        "seconds": len(chunk),
        "fps": round(statistics.mean(s["render"]["fps"] for s in chunk), 2),
        "videoMisses": sum(s["render"]["videoMisses"] for s in chunk),
        "skipped": sum(s["render"]["skipped"] for s in chunk),
        "maxGapMs": round(max(s["render"].get("maxGapMs", 0.0) for s in chunk), 1),
        "hitches": sum(o["hitches"] for o in outputs),
    }


def recovery(stats, at):
    for s in window(stats, at, at + 120):
        if s["render"]["fps"] >= 59:
            return s["unix"] - at
    return None


def main():
    stats = [s for s in lines(sys.argv[1]) if "unix" in s and "render" in s]
    events = lines(sys.argv[2])
    if not events:
        sys.exit("no events")
    start, end = events[0]["unix"], events[-1]["unix"]
    session = window(stats, start, end + 1)
    report = {"session": summary(session), "events": []}
    for e in events:
        at = e["unix"]
        report["events"].append({
            "event": e["event"],
            "detail": e.get("detail", ""),
            "before": summary(window(stats, at - 10, at)),
            "after": summary(window(stats, at, at + 60)),
            "backAt59fpsS": recovery(stats, at),
        })
    json.dump(report, sys.stdout, indent=2)
    print()
    s = report["session"]
    if s:
        print(f"session {s['seconds']} s: {s['fps']} fps, {s['videoMisses']} misses, {s['skipped']} skipped, "
              f"{s['hitches']} hitches, longest gap {s['maxGapMs']} ms", file=sys.stderr)
    for e in report["events"]:
        a = e["after"] or {}
        print(f"  {e['event']:16} back at 59 fps after {e['backAt59fpsS']} s; next minute {a.get('fps')} fps, "
              f"{a.get('videoMisses')} misses, {a.get('hitches')} hitches", file=sys.stderr)


if __name__ == "__main__":
    main()
