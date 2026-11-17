"""汇总 memcurve.jsonl：每个应用、配置、N 的总 PSS 与分项（MiB），取各次重复的平均。"""
from __future__ import annotations

import json
import sys
from collections import defaultdict


def main(path):
    rows = defaultdict(list)
    for line in open(path):
        r = json.loads(line)
        rows[(r["app"], r["config"], r["n"])].append(r)
    M = 1024
    print("| app | config | N | runs | windows | total PSS | per agent | anon | file | shmem | "
          "compositor PSS | compositor excl. shm | swap |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    order = {"shared": 0, "per-comp": 1, "per-usr": 2}
    for (app, cfg, n), rs in sorted(rows.items(), key=lambda kv: (kv[0][0], kv[0][2], order[kv[0][1]])):
        avg = lambda f: sum(f(r) for r in rs) / len(rs) / M  # noqa: E731
        tot = avg(lambda r: r["total"]["Pss"])
        comp = avg(lambda r: r["compositor"]["Pss"])
        comp_own = avg(lambda r: r["compositor"]["Pss"] - r["compositor"]["Pss_Shmem"])
        win = sum(r["windows_up"] for r in rs) / len(rs)
        print(f"| {app} | {cfg} | {n} | {len(rs)} | {win:.0f}/{n} | {tot:.0f} | {tot / n:.0f} | "
              f"{avg(lambda r: r['total']['Pss_Anon']):.0f} | {avg(lambda r: r['total']['Pss_File']):.0f} | "
              f"{avg(lambda r: r['total']['Pss_Shmem']):.0f} | {comp:.0f} | {comp_own:.0f} | "
              f"{avg(lambda r: r['total']['SwapPss']):.0f} |")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "runs/memcurve.jsonl")
