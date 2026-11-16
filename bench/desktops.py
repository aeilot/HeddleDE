"""被测桌面：现成的 sway 无头模式，或 Heddle。统一的启动、动作、截图接口。"""
from __future__ import annotations

import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "control"))
from heddle import Heddle  # noqa: E402


def _wait(pred, timeout, what):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        r = pred()
        if r:
            return r
        time.sleep(0.05)
    raise TimeoutError(what)


def sha(path: str) -> str:
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()[:16]


class Desktop:
    name = "?"
    wayland_display: str
    proc: subprocess.Popen

    def env(self, xwayland: bool = False) -> dict:
        e = dict(os.environ)
        e["WAYLAND_DISPLAY"] = self.wayland_display
        e["XDG_RUNTIME_DIR"] = self.runtime
        e.pop("DISPLAY", None)
        if xwayland and getattr(self, "x_display", None):
            e["DISPLAY"] = self.x_display
        return e

    def launch(self, cmd: str, xwayland: bool = False, isolate: bool = False) -> subprocess.Popen:
        argv = ["setsid", "sh", "-c", cmd]
        if isolate:
            sock = os.path.join(self.runtime, self.wayland_display)
            argv = [os.path.join(ROOT, "scripts", "heddle-run"), "--agent", self.agent_name(),
                    "--state", os.path.join(self.runtime, "state"),
                    "--socket", sock, "--"] + ["sh", "-c", cmd]
            argv = ["setsid"] + argv
        return subprocess.Popen(argv, env=self.env(xwayland), stdout=subprocess.DEVNULL,
                                stderr=open(os.path.join(self.runtime, "app.log"), "ab"))

    def agent_name(self) -> str:
        return "a0"

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        shutil.rmtree(self.runtime, ignore_errors=True)


class Sway(Desktop):
    """sway 无头模式。power_off=True 时观测之间关闭输出（门槛实验）。"""

    def __init__(self, size="1920x1080", power_off=False, xwayland=True, grace_ms=2000):
        self.name = "sway-poweroff" if power_off else "sway"
        self.power_off = power_off
        self.grace_ms = grace_ms
        self.runtime = tempfile.mkdtemp(prefix="heddle-sway-")
        os.chmod(self.runtime, 0o700)
        w, h = size.split("x")
        cfg = os.path.join(self.runtime, "config")
        with open(cfg, "w") as f:
            f.write(f"output HEADLESS-1 resolution {w}x{h} position 0 0\n"
                    f"xwayland {'enable' if xwayland else 'disable'}\n"
                    "default_border none\nfocus_follows_mouse no\n"
                    "seat * hide_cursor 0\n")
        env = dict(os.environ, XDG_RUNTIME_DIR=self.runtime, WLR_BACKENDS="headless",
                   WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1")
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DISPLAY", None)
        self.proc = subprocess.Popen(["sway", "-c", cfg], env=env, stdout=subprocess.DEVNULL,
                                     stderr=open(os.path.join(self.runtime, "sway.log"), "wb"),
                                     start_new_session=True)
        sock = _wait(lambda: glob.glob(os.path.join(self.runtime, "wayland-*[0-9]")), 10, "sway socket")
        self.wayland_display = os.path.basename(sock[0])
        self.swaysock = _wait(lambda: glob.glob(os.path.join(self.runtime, "sway-ipc.*.sock")), 10, "sway ipc")[0]
        self.x_display = None
        if xwayland:
            # sway 按需启动 Xwayland；显示号由第一个空闲的 /tmp/.X11-unix/X* 决定，启动后从日志读。
            self.x_display = ":0"
        self.sid = self.proc.pid
        self._power = True
        self.output = "HEADLESS-1"

    def msg(self, *args, json_out=False):
        r = subprocess.run(["swaymsg", "-s", self.swaysock, *(["-r"] if json_out else []), *args],
                           capture_output=True, text=True)
        if json_out:
            return json.loads(r.stdout or "null")
        return r.stdout

    def set_power(self, on: bool):
        if self._power != on:
            self.msg("output", self.output, "power", "on" if on else "off")
            self._power = on

    def windows(self):
        out = []

        def walk(n):
            if n.get("pid") and n.get("type") in ("con", "floating_con"):
                r = n["rect"]
                out.append({"pid": n["pid"], "app_id": n.get("app_id"), "shell": n.get("shell"),
                            "title": n.get("name"), "x": r["x"], "y": r["y"],
                            "width": r["width"], "height": r["height"], "focused": n.get("focused")})
            for c in n.get("nodes", []) + n.get("floating_nodes", []):
                walk(c)
        walk(self.msg("-t", "get_tree", json_out=True) or {})
        return out

    def action(self, spec: str):
        kind, _, arg = spec.partition(":")
        env = self.env()
        if kind == "type":
            subprocess.run(["wtype", arg], env=env, check=False)
        elif kind == "key":
            parts = arg.split("+")
            mods = {"ctrl": "ctrl", "shift": "shift", "alt": "alt", "super": "logo"}
            argv = ["wtype"]
            for m in parts[:-1]:
                argv += ["-M", mods.get(m.lower(), m)]
            argv += ["-k", parts[-1]]
            for m in reversed(parts[:-1]):
                argv += ["-m", mods.get(m.lower(), m)]
            subprocess.run(argv, env=env, check=False)
        elif kind == "click":
            x, y = arg.split(",")
            self.msg("seat", "-", "cursor", "set", x, y)
            self.msg("seat", "-", "cursor", "press", "button1")
            self.msg("seat", "-", "cursor", "release", "button1")
        elif kind == "none":
            pass
        else:
            raise ValueError(spec)

    def after_action(self):
        # 门槛实验：动作后放行 grace，再关输出。
        if self.power_off:
            time.sleep(self.grace_ms / 1000)
            self.set_power(False)

    def before_action(self):
        if self.power_off:
            self.set_power(True)

    def screenshot(self, path: str, settle_ms=300, timeout_ms=5000) -> dict:
        t0 = time.monotonic()
        if self.power_off:
            self.set_power(True)
            # 打开输出后应用才会收到帧回调并重画；sway 没有"画面稳定"信号，固定等 settle_ms。
            time.sleep(settle_ms / 1000)
        try:
            subprocess.run(["grim", "-o", self.output, path], env=self.env(), check=True,
                           timeout=timeout_ms / 1000)
        except (subprocess.TimeoutExpired, subprocess.CalledProcessError) as e:
            # 已观察到：sway 1.10.1 无头 + pixman 下 power off 再 on 后 page-flip 失败，grim 不返回。
            return {"status": "error", "error": type(e).__name__,
                    "latency_ms": round((time.monotonic() - t0) * 1000), "hash": None}
        return {"status": "fixed-wait" if self.power_off else "immediate",
                "latency_ms": round((time.monotonic() - t0) * 1000), "hash": sha(path)}

    def frame_stats(self):
        return None


class HeddleDesktop(Desktop):
    def __init__(self, binary: str, size="1920x1080", policy="always", grace_ms=2000, suspend=False,
                 agents=("a0",)):
        self.name = f"heddle-{policy}{'-suspend' if suspend else ''}"
        self.runtime = tempfile.mkdtemp(prefix="heddle-")
        os.chmod(self.runtime, 0o700)
        hr = os.path.join(self.runtime, "heddle")
        argv = [binary, "-R", hr, "-s", size, "-p", policy, "-g", str(grace_ms)]
        if suspend:
            argv.append("-S")
        for a in agents:
            argv += ["-a", a]
        self.proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                                     stderr=open(os.path.join(self.runtime, "heddle.log"), "wb"),
                                     start_new_session=True)
        _wait(lambda: os.path.exists(os.path.join(hr, "ready")), 10, "heddle ready")
        self.hr = hr
        self.agent = agents[0]
        self.wayland_display = os.path.join(hr, self.agent, "wayland-0")
        self.h = Heddle(os.path.join(hr, "control"), self.agent)
        self.sid = self.proc.pid

    def agent_name(self):
        return self.agent

    def windows(self):
        return self.h.windows()

    def action(self, spec: str):
        kind, _, arg = spec.partition(":")
        if kind == "type":
            self.h.type(arg)
        elif kind == "key":
            self.h.key(arg)
        elif kind == "click":
            x, y = arg.split(",")
            self.h.click(float(x), float(y))
        elif kind == "none":
            pass
        else:
            raise ValueError(spec)

    def before_action(self):
        pass

    def after_action(self):
        pass

    def screenshot(self, path: str, settle_ms=300, timeout_ms=5000) -> dict:
        r = self.h.screenshot(path, settle_ms=settle_ms, timeout_ms=timeout_ms)
        return {"status": r["status"], "latency_ms": r["latency_ms"], "frames": r["frames"],
                "hash": sha(path)}

    def frame_stats(self):
        return self.h.stats(reset=True)

    def stop(self):
        try:
            self.h.close()
        except Exception:  # noqa: BLE001
            pass
        super().stop()
