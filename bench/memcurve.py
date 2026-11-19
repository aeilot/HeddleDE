"""第 4 步：内存曲线。同时运行 N 个 agent（每个一个应用，经 heddle-run 隔离），测总内存。

配置：
  shared       一个 Heddle 合成器服务全部 agent，所有 agent 共用只读 /usr（Heddle 的设计）
  per-comp     每个 agent 一个独立的 Heddle 进程（对照：每 agent 一个最小合成器）
  per-usr      一个合成器，但每个 agent 绑定自己的一份 /usr 拷贝（对照：不共享文件页，
               近似每个 agent 一个独立镜像）

主指标是容器内所有相关进程的 PSS 之和：共享页按映射进程数分摊，所以总和就是这些进程
实际占用的常驻内存（不含内核内存、未映射的 tmpfs 和页缓存）。

  python3 bench/memcurve.py --heddle /tmp/heddle-build/heddle --app chromium --config shared -n 4
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "control"))
sys.path.insert(0, HERE)
from heddle import Heddle  # noqa: E402
from procstat import mem_kib  # noqa: E402

PAGES = os.path.join(HERE, "pages")
APPS = {
    "chromium": ("chromium --no-sandbox --ozone-platform=wayland --disable-gpu --no-first-run "
                 "--no-default-browser-check --password-store=basic "
                 f"--user-data-dir=/tmp/chromium-profile file://{PAGES}/form.html"),
    "textedit": "gnome-text-editor --standalone",
    # 逐项关掉的对照：GSK 用 cairo 软件渲染（不走 GL/llvmpipe）；关掉 GTK4 的无障碍
    "textedit-cairo": "env GSK_RENDERER=cairo gnome-text-editor --standalone",
    "textedit-noa11y": "env GTK_A11Y=none gnome-text-editor --standalone",
    "textedit-cairo-noa11y": "env GSK_RENDERER=cairo GTK_A11Y=none gnome-text-editor --standalone",
    "writer": "env SAL_USE_VCLPLUGIN=gtk3 soffice --writer --norestore --nologo",
}
ACTIONS = ["type:Agent notes for the quarterly review", "key:Return", "type:Second line of text"]
KEYS = ("Rss", "Pss", "Pss_Anon", "Pss_File", "Pss_Shmem", "SwapPss")


def proc_table():
    """pid -> (ppid, comm)"""
    out = {}
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            with open(f"/proc/{d}/stat") as f:
                s = f.read()
        except OSError:
            continue
        rp = s.rindex(")")
        out[int(d)] = (int(s[rp + 2:].split()[1]), s[s.index("(") + 1:rp])
    return out


def descendants(root, table):
    kids = {}
    for pid, (ppid, _) in table.items():
        kids.setdefault(ppid, []).append(pid)
    out, stack = [], [root]
    while stack:
        p = stack.pop()
        if p in table:
            out.append(p)
        stack.extend(kids.get(p, []))
    return out


def mem_of(pids, table):
    tot = dict.fromkeys(KEYS, 0)
    by_comm = {}
    for pid in pids:
        m = mem_kib(pid)
        for k in KEYS:
            tot[k] += m.get(k, 0)
        c = table[pid][1]
        by_comm[c] = by_comm.get(c, 0) + m.get("Pss", 0)
    return tot, by_comm


def meminfo():
    r = {}
    with open("/proc/meminfo") as f:
        for line in f:
            k, _, v = line.partition(":")
            r[k] = int(v.split()[0])
    return r


def wait(pred, timeout):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            if pred():
                return True
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.25)
    return False


def kill_tree(root):
    for sig in (15, 9):
        pids = descendants(root, proc_table())
        for pid in pids:
            try:
                os.kill(pid, sig)
            except OSError:
                pass
        t0 = time.monotonic()
        while time.monotonic() - t0 < 4 and any(os.path.exists(f"/proc/{p}") for p in pids):
            time.sleep(0.1)


def ensure_usr_copies(base, n):
    os.makedirs(base, exist_ok=True)
    for i in range(n):
        d = os.path.join(base, f"a{i}")
        if not os.path.exists(os.path.join(d, ".done")):
            shutil.rmtree(d, ignore_errors=True)
            print(f"  copying /usr -> {d}", file=sys.stderr)
            subprocess.run(["cp", "-a", "/usr", d], check=True)
            open(os.path.join(d, ".done"), "w").close()


def run_once(a):
    agents = [f"a{i}" for i in range(a.n)]
    rt = tempfile.mkdtemp(prefix="heddle-mem-")
    os.chmod(rt, 0o700)
    env = dict(os.environ, XDG_RUNTIME_DIR=rt)
    base_avail = meminfo()["MemAvailable"]

    # 合成器
    comps = []  # (Popen, runtime dir, [agents])
    groups = [agents] if a.config != "per-comp" else [[x] for x in agents]
    for gi, g in enumerate(groups):
        hr = os.path.join(rt, f"hd{gi}")
        argv = [a.heddle, "-R", hr, "-s", a.size, "-p", a.policy]
        for x in g:
            argv += ["-a", x]
        p = subprocess.Popen(argv, stdout=subprocess.DEVNULL, env=env,
                             stderr=open(os.path.join(rt, f"heddle{gi}.log"), "wb"),
                             start_new_session=True)
        if not wait(lambda hr=hr: os.path.exists(os.path.join(hr, "ready")), 10):
            raise RuntimeError("heddle not ready")
        comps.append((p, hr, g))
    where = {x: hr for _, hr, g in comps for x in g}
    ctl = {x: Heddle(os.path.join(where[x], "control"), x) for x in agents}

    # 应用：每个 agent 一个 heddle-run 沙盒
    launchers = {}
    for x in agents:
        argv = [os.path.join(ROOT, "scripts", "heddle-run"), "--agent", x, "--fresh",
                "--state", os.path.join(rt, "state"), "--socket", os.path.join(where[x], x, "wayland-0"),
                "--ro", PAGES]
        if a.config == "per-usr":
            argv += ["--usr", os.path.join(a.usr_copies, x)]
        argv += ["--"] + shlex.split(APPS[a.app])
        launchers[x] = subprocess.Popen(argv, env=env, stdout=subprocess.DEVNULL,
                                        stderr=open(os.path.join(rt, f"app-{x}.log"), "wb"),
                                        start_new_session=True)
        time.sleep(a.stagger)

    up = {x: wait(lambda x=x: len(ctl[x].windows()) > 0, a.window_timeout) for x in agents}
    time.sleep(a.settle)
    shots = {}
    for x in agents:
        for act in ACTIONS:
            kind, _, arg = act.partition(":")
            (ctl[x].type if kind == "type" else ctl[x].key)(arg)
        r = ctl[x].screenshot(os.path.join(rt, f"{x}.png"))
        shots[x] = r.get("status")
    time.sleep(a.after)

    table = proc_table()
    rec = {"app": a.app, "config": a.config, "n": a.n, "policy": a.policy, "label": a.label,
           "windows_up": sum(up.values()), "shots": shots}
    comp_pids = [p.pid for p, _, _ in comps]
    rec["compositor"], _ = mem_of(comp_pids, table)
    per_agent = {}
    all_by_comm = {}
    for x in agents:
        pids = descendants(launchers[x].pid, table)
        tot, by_comm = mem_of(pids, table)
        per_agent[x] = {"procs": len(pids), **tot}
        for c, v in by_comm.items():
            all_by_comm[c] = all_by_comm.get(c, 0) + v
    rec["agents"] = per_agent
    rec["apps_total"] = {k: sum(v[k] for v in per_agent.values()) for k in KEYS}
    rec["total"] = {k: rec["apps_total"][k] + rec["compositor"][k] for k in KEYS}
    rec["by_comm_pss"] = dict(sorted(all_by_comm.items(), key=lambda kv: -kv[1]))
    rec["mem_available_delta_kib"] = base_avail - meminfo()["MemAvailable"]

    for x in agents:
        ctl[x].close()
        kill_tree(launchers[x].pid)
    for p, _, _ in comps:
        kill_tree(p.pid)
    shutil.rmtree(rt, ignore_errors=True)
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--heddle", required=True)
    ap.add_argument("--app", choices=sorted(APPS), required=True)
    ap.add_argument("--config", choices=["shared", "per-comp", "per-usr"], default="shared")
    ap.add_argument("-n", type=int, default=1)
    ap.add_argument("--policy", default="observe")
    ap.add_argument("--size", default="1920x1080")
    ap.add_argument("--stagger", type=float, default=1.0)
    ap.add_argument("--window-timeout", type=float, default=120)
    ap.add_argument("--settle", type=float, default=15)
    ap.add_argument("--after", type=float, default=10)
    ap.add_argument("--usr-copies", default="/var/tmp/heddle-usr")
    ap.add_argument("--label", default="")
    ap.add_argument("--out", default="runs/memcurve.jsonl")
    a = ap.parse_args()
    if a.config == "per-usr":
        ensure_usr_copies(a.usr_copies, a.n)
    rec = run_once(a)
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "a") as f:
        f.write(json.dumps(rec) + "\n")
    mib = lambda k: rec["total"][k] / 1024  # noqa: E731
    print(f"{a.app} {a.config} n={a.n}: windows {rec['windows_up']}/{a.n} "
          f"total PSS {mib('Pss'):.0f} MiB (anon {mib('Pss_Anon'):.0f}, file {mib('Pss_File'):.0f}, "
          f"shmem {mib('Pss_Shmem'):.0f}) compositor {rec['compositor']['Pss'] / 1024:.0f} MiB "
          f"swap {mib('SwapPss'):.0f}", file=sys.stderr)


if __name__ == "__main__":
    main()
