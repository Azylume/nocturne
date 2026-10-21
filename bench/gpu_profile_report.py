#!/usr/bin/env python3
"""GPU time by layer, fade side and pass from an engine's --stats-json file recorded with --gpu-profile.

usage: bench/gpu_profile_report.py STATS.jsonl [--passes]

Seconds with no layer in a transition and seconds with one or more are averaged apart. A row is milliseconds per frame
that drew it: "L3" is layer 3 outside a transition, "L3 in" and "L3 out" its incoming and outgoing clip during one
("in" includes the mix), "stack" the clears of the composition and group stacks. --passes adds every pass by effects.
"""
import json
import sys
from collections import defaultdict


def main():
    path = sys.argv[1]
    passes = "--passes" in sys.argv
    sums = {False: defaultdict(lambda: [0.0, 0]), True: defaultdict(lambda: [0.0, 0])}
    gpu = {False: [], True: []}
    budget = {False: [], True: []}
    for line in open(path):
        stats = json.loads(line)
        render = stats.get("render", {})
        fading = stats.get("sizes", {}).get("fading", 0) > 0
        gpu[fading].append(render.get("gpuMs", 0.0))
        budget[fading].append(render.get("budgetLevel", 0))
        for label, value in render.get("gpuProfile", {}).items():
            if passes or " | " not in label:
                total = sums[fading][label]
                total[0] += value["ms"] * value["frames"]
                total[1] += value["frames"]

    def avg(values):
        return sum(values) / len(values) if values else 0.0

    print(f"seconds: steady {len(gpu[False])}, in transition {len(gpu[True])}")
    print(f"gpu ms/frame: steady {avg(gpu[False]):.2f}, in transition {avg(gpu[True]):.2f}")
    print(f"budget level: steady {avg(budget[False]):.2f}, in transition {avg(budget[True]):.2f}")
    labels = set(sums[False]) | set(sums[True])

    def ms(fading, label):
        total, frames = sums[fading].get(label, (0.0, 0))
        return total / frames if frames else None

    rows = sorted(labels, key=lambda label: (" | " in label, -(ms(True, label) or ms(False, label) or 0.0)))
    print(f"{'label':48} {'steady':>8} {'in transition':>14}")
    for label in rows:
        cells = [ms(False, label), ms(True, label)]
        text = [f"{c:8.2f}" if c is not None else f"{'-':>8}" for c in cells]
        print(f"{label:48} {text[0]} {text[1]:>14}")


if __name__ == "__main__":
    main()
