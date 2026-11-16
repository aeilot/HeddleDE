"""把 run.py 的 JSONL 汇总成一张表：每个 (应用, 桌面) 一行。"""
from __future__ import annotations

import json
import sys
from collections import defaultdict


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else float("nan")


def main(paths):
    groups = defaultdict(list)
    for p in paths:
        with open(p) as f:
            for line in f:
                r = json.loads(line)
                groups[(r["label"], r["desktop"])].append(r)
    hdr = ("app", "desktop", "steps", "app_cpu_0-10s", "app_cpu_rest", "app_step_cores", "desk_step_cores", "shot_ms",
           "timeouts/errors", "stale", "frames/step", "app_pss_MiB", "desk_pss_MiB")
    print("| " + " | ".join(hdr) + " |")
    print("|" + "---|" * len(hdr))
    for (label, desk), recs in sorted(groups.items()):
        steps = [s for r in recs for s in r["steps"]]
        k10 = next(iter(steps[0]["cpu"]["app"])) if steps else "first_10s"
        frames = [s["frames"]["frames_rendered"] for s in steps if "frames" in s]
        stale = [s["stale"] for s in steps if s.get("stale") is not None]
        row = (label, desk, len(steps),
               f"{mean([s['cpu']['app'][k10] for s in steps]):.3f}",
               f"{mean([s['cpu']['app']['rest'] for s in steps]):.3f}",
               f"{mean([s.get('step_cores', {}).get('app') for s in steps]):.3f}",
               f"{mean([s.get('step_cores', {}).get('desktop') for s in steps]):.3f}",
               f"{mean([s['shot']['latency_ms'] for s in steps]):.0f}",
               "%d/%d" % (sum(1 for s in steps if s["shot"]["status"] == "timeout"),
                          sum(1 for s in steps if s["shot"]["status"] == "error")),
               f"{sum(stale)}/{len(stale)}" if stale else "-",
               f"{mean(frames):.1f}" if frames else "-",
               f"{mean([r['mem_app']['total_kib'].get('Pss', 0) / 1024 for r in recs]):.0f}",
               f"{mean([r['mem_desktop']['total_kib'].get('Pss', 0) / 1024 for r in recs]):.0f}")
        print("| " + " | ".join(str(x) for x in row) + " |")


if __name__ == "__main__":
    main(sys.argv[1:] or ["runs/results.jsonl"])
