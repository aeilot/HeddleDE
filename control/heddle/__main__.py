"""命令行：python -m heddle [--agent A] <命令> ...

  agents | windows | stats [--reset] | screenshot PATH [--settle MS] [--timeout MS]
  click X Y [--button B] [--count N] | move X Y | scroll DX DY
  key COMBO | type TEXT | run MS | gate | pyautogui 'CODE' | serve [--port P]
"""
from __future__ import annotations

import argparse
import json
import sys

from .client import Heddle


def main(argv=None):
    p = argparse.ArgumentParser(prog="heddle")
    p.add_argument("--control", help="control socket path")
    p.add_argument("--agent", "-a", default="a0")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("agents")
    sub.add_parser("windows")
    s = sub.add_parser("stats"); s.add_argument("--reset", action="store_true")
    s = sub.add_parser("screenshot"); s.add_argument("path")
    s.add_argument("--settle", type=int, default=300); s.add_argument("--timeout", type=int, default=5000)
    s.add_argument("--threshold", type=int, default=4096)
    s = sub.add_parser("click"); s.add_argument("x", type=float); s.add_argument("y", type=float)
    s.add_argument("--button", default="left"); s.add_argument("--count", type=int, default=1)
    s = sub.add_parser("move"); s.add_argument("x", type=float); s.add_argument("y", type=float)
    s = sub.add_parser("scroll"); s.add_argument("dx", type=int); s.add_argument("dy", type=int)
    s = sub.add_parser("key"); s.add_argument("combo")
    s = sub.add_parser("type"); s.add_argument("text")
    s = sub.add_parser("run"); s.add_argument("ms", type=int)
    sub.add_parser("gate")
    s = sub.add_parser("pyautogui"); s.add_argument("code")
    s = sub.add_parser("serve"); s.add_argument("--host", default="127.0.0.1"); s.add_argument("--port", type=int, default=5000)
    a = p.parse_args(argv)

    if a.cmd == "serve":
        from .server import serve
        serve(a.control, a.agent, a.host, a.port)
        return
    h = Heddle(a.control, a.agent)
    if a.cmd == "agents": r = h.agents()
    elif a.cmd == "windows": r = h.windows()
    elif a.cmd == "stats": r = h.stats(reset=a.reset)
    elif a.cmd == "screenshot": r = h.screenshot(a.path, settle_ms=a.settle, timeout_ms=a.timeout, threshold_px=a.threshold)
    elif a.cmd == "click": r = h.click(a.x, a.y, a.button, a.count)
    elif a.cmd == "move": r = h.move(a.x, a.y)
    elif a.cmd == "scroll": r = h.scroll(a.dx, a.dy)
    elif a.cmd == "key": r = h.key(a.combo)
    elif a.cmd == "type": r = h.type(a.text)
    elif a.cmd == "run": r = h.run(a.ms)
    elif a.cmd == "gate": r = h.gate()
    elif a.cmd == "pyautogui":
        import time
        from . import pyautogui_shim
        pyautogui_shim.bind(h)
        exec(a.code, {"pyautogui": pyautogui_shim, "time": time})
        r = {"ok": True}
    json.dump(r, sys.stdout, ensure_ascii=False)
    print()


if __name__ == "__main__":
    main()
