"""一次实验：在指定桌面上启动应用，按"动作 → 等待（模型思考）→ 截图"循环若干步。

每步记录：动作后按进程的 CPU（逐秒）、截图状态与延迟、每步帧数（Heddle）、
截图是否过时（--check-stale：紧接着用长 settle 再截一张比较哈希）。结束时记录内存。

  python3 bench/run.py --desktop sway --app foot --steps 5 --wait 24 --action type:hello
  python3 bench/run.py --desktop sway-poweroff ...          # 门槛实验
  python3 bench/run.py --desktop heddle-observe --heddle build/heddle ...  # 第 3 步
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from desktops import HeddleDesktop, Sway, sha  # noqa: E402
from procstat import CpuSampler, session_cpu_seconds, session_mem  # noqa: E402


def kill_session(sid, grace=3.0):
    """结束应用会话里的所有进程（含 setsid 之后仍留在会话里的子进程）。"""
    from procstat import session_pids
    for sig in (15, 9):
        for pid in session_pids(sid):
            try:
                os.kill(pid, sig)
            except OSError:
                pass
        t0 = time.monotonic()
        while session_pids(sid) and time.monotonic() - t0 < grace:
            time.sleep(0.1)


def pixel_diff(a_path, b_path):
    """两张截图不同的像素数与外接框；没有 PIL 时只比哈希。"""
    try:
        from PIL import Image, ImageChops
    except ImportError:
        return {"px": 0 if sha(a_path) == sha(b_path) else 1 << 30, "bbox": None}
    a, b = Image.open(a_path).convert("RGB"), Image.open(b_path).convert("RGB")
    if a.size != b.size:
        return {"px": a.size[0] * a.size[1], "bbox": None}
    d = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v else 0)
    return {"px": sum(1 for v in d.getdata() if v), "bbox": d.getbbox()}


def summarize_cpu(rows, split_s=10.0):
    def avg(sel, key):
        vals = [r[key]["cores"] for r in sel]
        return round(sum(vals) / len(vals), 4) if vals else None
    early = [r for r in rows if r["t"] <= split_s]
    late = [r for r in rows if r["t"] > split_s]
    out = {}
    for key in ("app", "desktop"):
        out[key] = {"first_%ds" % split_s: avg(early, key), "rest": avg(late, key), "all": avg(rows, key)}
    return out


def make_desktop(a):
    if a.desktop == "sway":
        return Sway(a.size, power_off=False, xwayland=a.xwayland)
    if a.desktop == "sway-poweroff":
        return Sway(a.size, power_off=True, xwayland=a.xwayland, grace_ms=a.grace)
    if a.desktop.startswith("heddle-"):
        policy = a.desktop.split("-")[1]
        return HeddleDesktop(a.heddle, a.size, policy=policy, grace_ms=a.grace,
                             suspend=a.desktop.endswith("-suspend"))
    raise SystemExit(f"unknown desktop {a.desktop}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--desktop", required=True,
                   help="sway | sway-poweroff | heddle-always | heddle-observe | heddle-observe-suspend")
    p.add_argument("--heddle", default=os.environ.get("HEDDLE_BIN", "build/heddle"))
    p.add_argument("--app", required=True, help="shell command to start the application")
    p.add_argument("--label", default=None)
    p.add_argument("--size", default="1920x1080")
    p.add_argument("--xwayland", action="store_true", help="allow X11 clients (sway only)")
    p.add_argument("--isolate", action="store_true", help="launch the app through scripts/heddle-run")
    p.add_argument("--warmup", type=float, default=10)
    p.add_argument("--steps", type=int, default=5)
    p.add_argument("--wait", type=float, default=24, help="seconds between actions (median model wait)")
    p.add_argument("--action", action="append", default=[],
                   help="type:TEXT | key:COMBO | click:X,Y | none (cycled across steps)")
    p.add_argument("--grace", type=int, default=2000)
    p.add_argument("--settle", type=int, default=300)
    p.add_argument("--timeout", type=int, default=5000)
    p.add_argument("--check-stale", action="store_true")
    p.add_argument("--stale-px", type=int, default=4096, help="changed pixels above which a shot counts as stale")
    p.add_argument("--out", default="runs/results.jsonl")
    p.add_argument("--shots", default=None, help="directory to keep screenshots")
    a = p.parse_args()
    actions = a.action or ["none"]

    d = make_desktop(a)
    shots = a.shots or os.path.join(d.runtime, "shots")
    os.makedirs(shots, exist_ok=True)
    rec = {"label": a.label or a.app, "desktop": d.name, "app": a.app, "size": a.size,
           "wait_s": a.wait, "grace_ms": a.grace, "started": time.time(), "steps": []}
    try:
        app = d.launch(a.app, xwayland=a.xwayland, isolate=a.isolate)
        sampler = CpuSampler({"app": app.pid, "desktop": d.sid})
        time.sleep(a.warmup)
        rec["windows"] = d.windows()
        rec["app_alive"] = app.poll() is None
        d.frame_stats()  # 清零
        for i in range(a.steps):
            step = {"i": i}
            spec = actions[i % len(actions)]
            c0 = {"app": session_cpu_seconds(app.pid), "desktop": session_cpu_seconds(d.sid)}
            w0 = time.monotonic()
            d.before_action()
            t0 = time.monotonic()
            d.action(spec)
            step["action"] = spec
            step["action_ms"] = round((time.monotonic() - t0) * 1000)
            d.after_action()
            rows = sampler.sample(a.wait)
            step["cpu"] = summarize_cpu(rows)
            step["cpu_rows"] = rows
            path = os.path.join(shots, f"{d.name}-{i:03d}.png")
            step["shot"] = d.screenshot(path, a.settle, a.timeout)
            # 整步（动作 + 等待 + 截图）的平均核数：observe 把渲染推到截图时，只看等待期会少算。
            wall = time.monotonic() - w0
            step["step_wall_s"] = round(wall, 3)
            step["step_cores"] = {k: round((session_cpu_seconds(sid) - c0[k]) / wall, 4)
                                  for k, sid in (("app", app.pid), ("desktop", d.sid))}
            fs = d.frame_stats()
            if fs:
                step["frames"] = fs
            if a.check_stale:
                ref = os.path.join(shots, f"{d.name}-{i:03d}-ref.png")
                step["ref"] = d.screenshot(ref, 2000, 10000)
                ok = step["shot"]["hash"] and step["ref"]["hash"]
                if ok:
                    step["diff"] = pixel_diff(path, ref)
                    # 光标闪烁等小面积变化不算旧画面，阈值与稳定判定一致
                    step["stale"] = step["diff"]["px"] > a.stale_px
                else:
                    step["stale"] = None
                d.frame_stats()  # 丢弃参考截图期间的帧
            rec["steps"].append(step)
            print(json.dumps({k: v for k, v in step.items() if k != "cpu_rows"}), file=sys.stderr)
        rec["mem_app"] = session_mem(app.pid)
        rec["mem_desktop"] = session_mem(d.sid)
        rec["app_alive_end"] = app.poll() is None
    finally:
        d.stop()
        kill_session(app.pid)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "a") as f:
        f.write(json.dumps(rec) + "\n")
    print(f"wrote {a.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
