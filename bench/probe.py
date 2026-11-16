"""第 1 步：哪些应用能在纯 Wayland 下运行，哪些需要 Xwayland。

对每个应用命令，先在禁用 Xwayland 的 sway 里启动，看是否出现 xdg_shell 窗口；
不出现再在启用 Xwayland 的 sway 里试，记录窗口的 shell 类型。

  python3 bench/probe.py 'foot' 'gimp' 'vlc' 'code --ozone-platform=wayland'
"""
from __future__ import annotations

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from desktops import Sway  # noqa: E402


def try_app(cmd: str, xwayland: bool, timeout: float) -> dict:
    d = Sway(xwayland=xwayland)
    try:
        app = d.launch(cmd, xwayland=xwayland)
        t0 = time.monotonic()
        wins = []
        while time.monotonic() - t0 < timeout:
            wins = d.windows()
            if wins:
                break
            time.sleep(0.5)
        return {"xwayland_enabled": xwayland, "windows": wins,
                "shells": sorted({w.get("shell") for w in wins if w.get("shell")}),
                "startup_s": round(time.monotonic() - t0, 2) if wins else None,
                "exited": app.poll()}
    finally:
        d.stop()
        try:
            os.killpg(app.pid, 9)
        except Exception:  # noqa: BLE001
            pass


def main(cmds, timeout=30.0):
    for cmd in cmds:
        r = {"app": cmd, "wayland": try_app(cmd, False, timeout)}
        if not r["wayland"]["windows"]:
            r["xwayland"] = try_app(cmd, True, timeout)
        r["verdict"] = ("wayland" if r["wayland"]["windows"] else
                        "xwayland" if r.get("xwayland", {}).get("windows") else "no-window")
        print(json.dumps(r, ensure_ascii=False))


if __name__ == "__main__":
    main(sys.argv[1:])
