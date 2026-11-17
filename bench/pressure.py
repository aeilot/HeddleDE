"""第 4 步：内存压力下的缺页与截图延迟。

把合成器和 N 个 agent 的沙盒放进一个 cgroup v2（memory.max = 上限，memory.swap.max = 0，
只有文件页可回收），对比共享 /usr 与每 agent 一份 /usr 拷贝：启动耗时、主缺页、
文件页回收后重读（workingset_refault_file）、磁盘读、截图延迟、OOM。

每次运行前 sync 并清空页缓存，让所有文件页由本 cgroup 首次读入、计入本 cgroup 的用量
（否则之前读入的页记在别的 cgroup 上，会绕开上限）。需要 --privileged 容器。

  python3 bench/pressure.py --heddle /tmp/heddle-build/heddle --app chromium --config per-usr -n 8 --limit 2500
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from memcurve import APPS, PAGES, ROOT, Heddle, ensure_usr_copies, kill_tree, wait  # noqa: E402

CG = "/sys/fs/cgroup"
STAT_KEYS = ("pgmajfault", "pgfault", "workingset_refault_file", "workingset_refault_anon",
             "pgscan", "pgsteal", "anon", "file", "shmem", "kernel", "file_mapped")


def setup_cgroup(name, limit_mib):
    init = os.path.join(CG, "init")
    os.makedirs(init, exist_ok=True)
    for pid in open(os.path.join(CG, "cgroup.procs")).read().split():
        try:
            with open(os.path.join(init, "cgroup.procs"), "w") as f:
                f.write(pid)
        except OSError:
            pass
    with open(os.path.join(CG, "cgroup.subtree_control"), "w") as f:
        f.write("+memory +io")
    path = os.path.join(CG, name)
    if os.path.exists(path):
        os.rmdir(path)
    os.mkdir(path)
    with open(os.path.join(path, "memory.max"), "w") as f:
        f.write(str(limit_mib * 1024 * 1024) if limit_mib else "max")
    with open(os.path.join(path, "memory.swap.max"), "w") as f:
        f.write("0")
    return path


def read_kv(path):
    r = {}
    try:
        for line in open(path):
            k, _, v = line.strip().partition(" ")
            r[k] = int(v)
    except OSError:
        pass
    return r


def io_rbytes(cg):
    total = 0
    try:
        for line in open(os.path.join(cg, "io.stat")):
            for tok in line.split()[1:]:
                k, _, v = tok.partition("=")
                if k == "rbytes":
                    total += int(v)
    except OSError:
        pass
    return total


def snap(cg):
    st = read_kv(os.path.join(cg, "memory.stat"))
    s = {k: st.get(k, 0) for k in STAT_KEYS}
    s["rbytes"] = io_rbytes(cg)
    s["current"] = int(open(os.path.join(cg, "memory.current")).read())
    s["events"] = read_kv(os.path.join(cg, "memory.events"))
    return s


def delta(a, b):
    return {k: b[k] - a[k] for k in ("pgmajfault", "pgfault", "workingset_refault_file",
                                     "workingset_refault_anon", "pgscan", "pgsteal", "rbytes")}


def pct(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100 * (len(xs) - 1))))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--heddle", required=True)
    ap.add_argument("--app", choices=sorted(APPS), required=True)
    ap.add_argument("--config", choices=["shared", "per-usr"], default="shared")
    ap.add_argument("-n", type=int, default=8)
    ap.add_argument("--limit", type=int, default=0, help="MiB；0 表示不限")
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--wait", type=float, default=3.0, help="每轮之间的等待")
    ap.add_argument("--settle", type=float, default=10.0)
    ap.add_argument("--window-timeout", type=float, default=300)
    ap.add_argument("--shot-timeout", type=int, default=10000)
    ap.add_argument("--usr-copies", default="/var/tmp/heddle-usr")
    ap.add_argument("--label", default="")
    ap.add_argument("--out", default="runs/pressure.jsonl")
    a = ap.parse_args()

    agents = [f"a{i}" for i in range(a.n)]
    if a.config == "per-usr":
        ensure_usr_copies(a.usr_copies, a.n)
    cg = setup_cgroup("heddle-exp", a.limit)
    procs_file = os.path.join(cg, "cgroup.procs")

    def enter_cg():
        with open(procs_file, "w") as f:
            f.write(str(os.getpid()))

    subprocess.run(["sync"], check=True)
    with open("/proc/sys/vm/drop_caches", "w") as f:
        f.write("3")
    s_start = snap(cg)

    rt = tempfile.mkdtemp(prefix="heddle-pr-")
    os.chmod(rt, 0o700)
    env = dict(os.environ, XDG_RUNTIME_DIR=rt)
    hr = os.path.join(rt, "hd")
    argv = [a.heddle, "-R", hr, "-s", "1920x1080", "-p", "observe"]
    for x in agents:
        argv += ["-a", x]
    comp = subprocess.Popen(argv, stdout=subprocess.DEVNULL, env=env, preexec_fn=enter_cg,
                            stderr=open(os.path.join(rt, "heddle.log"), "wb"), start_new_session=True)
    if not wait(lambda: os.path.exists(os.path.join(hr, "ready")), 20):
        raise RuntimeError("heddle not ready")
    ctl = {x: Heddle(os.path.join(hr, "control"), x) for x in agents}

    launchers, started, up_at = {}, {}, {}

    def poll_windows(until):
        while time.monotonic() < until and len(up_at) < len(started):
            for x in started:
                if x not in up_at:
                    try:
                        if ctl[x].windows():
                            up_at[x] = time.monotonic() - started[x]
                    except Exception:  # noqa: BLE001
                        pass
            time.sleep(0.1)

    for x in agents:
        cmd = [os.path.join(ROOT, "scripts", "heddle-run"), "--agent", x, "--fresh",
               "--state", os.path.join(rt, "state"), "--socket", os.path.join(hr, x, "wayland-0"),
               "--ro", PAGES]
        if a.config == "per-usr":
            cmd += ["--usr", os.path.join(a.usr_copies, x)]
        cmd += ["--"] + shlex.split(APPS[a.app])
        launchers[x] = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, preexec_fn=enter_cg,
                                        stderr=open(os.path.join(rt, f"app-{x}.log"), "wb"),
                                        start_new_session=True)
        started[x] = time.monotonic()
        t_next = started[x] + 1.0
        poll_windows(t_next)
        time.sleep(max(0.0, t_next - time.monotonic()))
    poll_windows(time.monotonic() + a.window_timeout)
    s_up = snap(cg)
    time.sleep(a.settle)

    s0 = snap(cg)
    steps = []
    for r in range(a.rounds):
        for x in agents:
            t0 = time.monotonic()
            rec = {"round": r, "agent": x}
            try:
                ctl[x].type(f"Round {r} notes from agent {x}")
                ctl[x].key("Return")
                sh = ctl[x].screenshot(os.path.join(rt, f"{x}.png"), timeout_ms=a.shot_timeout)
                rec.update(status=sh.get("status"), latency_ms=sh.get("latency_ms"))
            except Exception as e:  # noqa: BLE001
                rec.update(status="error", error=str(e)[:200])
            rec["wall_ms"] = round((time.monotonic() - t0) * 1000)
            steps.append(rec)
        time.sleep(a.wait)
    s1 = snap(cg)
    alive = 0
    for x in agents:
        try:
            alive += 1 if ctl[x].windows() else 0
        except Exception:  # noqa: BLE001
            pass
    peak = int(open(os.path.join(cg, "memory.peak")).read())

    lat = [s["latency_ms"] for s in steps if s.get("latency_ms") is not None]
    wall = [s["wall_ms"] for s in steps]
    out = {
        "app": a.app, "config": a.config, "n": a.n, "limit_mib": a.limit, "label": a.label,
        "windows_up": len(up_at), "alive_end": alive,
        "startup_s": {"max": round(max(up_at.values()), 2) if up_at else None,
                      "median": round(statistics.median(up_at.values()), 2) if up_at else None},
        "startup": delta(s_start, s_up),
        "steady": delta(s0, s1),
        "shots": {"n": len(steps), "stable": sum(s["status"] == "stable" for s in steps),
                  "timeout": sum(s["status"] == "timeout" for s in steps),
                  "error": sum(s["status"] == "error" for s in steps),
                  "lat_p50": pct(lat, 50), "lat_p90": pct(lat, 90), "lat_max": max(lat) if lat else None,
                  "wall_p50": pct(wall, 50), "wall_p90": pct(wall, 90), "wall_max": max(wall) if wall else None},
        # memory.stat 的 file 含 shmem；file_cache 是去掉 shmem 后的普通页缓存
        "end": {**{k: s1[k] for k in ("anon", "file", "shmem", "file_mapped", "current")},
                "file_cache": s1["file"] - s1["shmem"]},
        "peak": peak,
        "events": s1["events"],
        "steps": steps,
    }

    for x in agents:
        ctl[x].close()
        kill_tree(launchers[x].pid)
    kill_tree(comp.pid)
    shutil.rmtree(rt, ignore_errors=True)
    t0 = time.monotonic()
    while time.monotonic() - t0 < 10:
        try:
            os.rmdir(cg)
            break
        except OSError:
            time.sleep(0.2)

    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "a") as f:
        f.write(json.dumps(out) + "\n")
    M = 1024 * 1024
    st, sh = out["steady"], out["shots"]
    print(f"{a.app} {a.config} n={a.n} limit={a.limit or 'none'}: up {len(up_at)}/{a.n} "
          f"in {out['startup_s']['max']}s, alive {alive}; peak {peak / M:.0f} MiB; "
          f"steady majflt {st['pgmajfault']} refault {st['workingset_refault_file']} "
          f"read {st['rbytes'] / M:.0f} MiB; shots p50 {sh['lat_p50']} p90 {sh['lat_p90']} "
          f"max {sh['lat_max']} ms, timeout {sh['timeout']} err {sh['error']}; "
          f"oom_kill {out['events'].get('oom_kill', 0)}", file=sys.stderr)


if __name__ == "__main__":
    main()
