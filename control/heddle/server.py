"""OSWorld 风格的 HTTP 控制服务（最小子集，待对照 OSWorld 源码核对接口）。

  GET  /screenshot            -> PNG（等画面稳定，头部 X-Heddle-Status: stable|timeout）
  POST /execute {"command": "pyautogui.click(1,2)"}  执行 pyautogui 动作片段
  GET  /windows               -> 窗口位置 JSON（供 AT-SPI 坐标换算）
  GET  /stats                 -> 帧与提交统计

只监听本机；/execute 会执行任意 Python，等同于 OSWorld 虚拟机内服务的信任模型。
"""
from __future__ import annotations

import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import pyautogui_shim
from .client import Heddle


def serve(control: str | None, agent: str, host: str, port: int) -> None:
    lock = threading.Lock()
    h = Heddle(control, agent)
    pyautogui_shim.bind(h)

    class Handler(BaseHTTPRequestHandler):
        def _json(self, code, obj):
            body = json.dumps(obj, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            try:
                with lock:
                    if self.path.startswith("/screenshot"):
                        png, meta = h.screenshot_bytes()
                        self.send_response(200)
                        self.send_header("Content-Type", "image/png")
                        self.send_header("Content-Length", str(len(png)))
                        self.send_header("X-Heddle-Status", meta["status"])
                        self.send_header("X-Heddle-Latency-Ms", str(meta["latency_ms"]))
                        self.end_headers()
                        self.wfile.write(png)
                    elif self.path.startswith("/windows"):
                        self._json(200, {"windows": h.windows()})
                    elif self.path.startswith("/stats"):
                        self._json(200, h.stats())
                    else:
                        self._json(404, {"error": "not found"})
            except Exception as e:  # noqa: BLE001
                self._json(500, {"error": str(e)})

        def do_POST(self):
            if not self.path.startswith("/execute"):
                self._json(404, {"error": "not found"})
                return
            n = int(self.headers.get("Content-Length", "0"))
            try:
                req = json.loads(self.rfile.read(n) or b"{}")
                code = req.get("command", "")
                if isinstance(code, list):
                    code = "\n".join(code)
                with lock:
                    exec(code, {"pyautogui": pyautogui_shim, "time": time})
                self._json(200, {"status": "success"})
            except NotImplementedError as e:
                self._json(501, {"status": "error", "message": str(e)})
            except Exception as e:  # noqa: BLE001
                self._json(500, {"status": "error", "message": str(e)})

        def log_message(self, fmt, *args):
            pass

    httpd = ThreadingHTTPServer((host, port), Handler)
    print(f"heddle control server on http://{host}:{port} (agent {agent})", flush=True)
    httpd.serve_forever()
