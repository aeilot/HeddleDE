"""合成器控制套接字的客户端（只用标准库）。

协议：每行一条文本命令，合成器回一行 JSON。见 compositor/README.md。
"""
from __future__ import annotations

import json
import os
import socket
import tempfile


class HeddleError(RuntimeError):
    pass


def default_control_path() -> str:
    rt = os.environ.get("HEDDLE_RUNTIME")
    if not rt:
        rt = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "heddle")
    return os.path.join(rt, "control")


class Heddle:
    def __init__(self, path: str | None = None, agent: str | None = None, timeout: float = 60.0):
        self.path = path or default_control_path()
        self.agent = agent
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(self.path)
        self._buf = b""

    def close(self) -> None:
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ---- 底层 ----
    def call(self, line: str) -> dict:
        if "\n" in line:
            raise ValueError("command must be a single line")
        self.sock.sendall(line.encode() + b"\n")
        while b"\n" not in self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise HeddleError("control socket closed")
            self._buf += chunk
        raw, self._buf = self._buf.split(b"\n", 1)
        reply = json.loads(raw)
        if not reply.get("ok"):
            raise HeddleError(reply.get("error", "unknown error"))
        return reply

    def _a(self, agent: str | None) -> str:
        a = agent or self.agent
        if not a:
            raise ValueError("agent not given")
        return a

    # ---- 命令 ----
    def agents(self) -> dict:
        return self.call("agents")

    def screenshot(self, path: str | None = None, agent: str | None = None, *,
                   settle_ms: int = 300, timeout_ms: int = 5000, threshold_px: int = 4096) -> dict:
        """截图写到 path（PNG）。返回 status=stable|timeout、latency_ms 等。"""
        if path is None:
            fd, path = tempfile.mkstemp(suffix=".png", prefix="heddle-")
            os.close(fd)
        path = os.path.abspath(path)
        if " " in path:
            raise ValueError("path must not contain spaces")
        return self.call(f"screenshot {self._a(agent)} {path} settle={settle_ms} "
                         f"timeout={timeout_ms} threshold={threshold_px}")

    def screenshot_bytes(self, agent: str | None = None, **kw) -> tuple[bytes, dict]:
        fd, path = tempfile.mkstemp(suffix=".png", prefix="heddle-")
        os.close(fd)
        try:
            meta = self.screenshot(path, agent, **kw)
            with open(path, "rb") as f:
                return f.read(), meta
        finally:
            os.unlink(path)

    def move(self, x: float, y: float, agent: str | None = None) -> dict:
        return self.call(f"move {self._a(agent)} {x} {y}")

    def click(self, x: float, y: float, button: str = "left", count: int = 1,
              agent: str | None = None) -> dict:
        return self.call(f"click {self._a(agent)} {x} {y} button={button} count={count}")

    def button(self, button: str, down: bool, agent: str | None = None) -> dict:
        return self.call(f"button {self._a(agent)} {button} {'down' if down else 'up'}")

    def scroll(self, dx: int = 0, dy: int = 0, agent: str | None = None) -> dict:
        return self.call(f"scroll {self._a(agent)} {dx} {dy}")

    def key(self, combo: str, agent: str | None = None) -> dict:
        """按一次组合键，如 'ctrl+s'、'Return'。"""
        return self.call(f"key {self._a(agent)} {combo}")

    def keydown(self, name: str, agent: str | None = None) -> dict:
        return self.call(f"keydown {self._a(agent)} {name}")

    def keyup(self, name: str, agent: str | None = None) -> dict:
        return self.call(f"keyup {self._a(agent)} {name}")

    def type(self, text: str, agent: str | None = None) -> dict:
        esc = text.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")
        return self.call(f"type {self._a(agent)} {esc}")

    def windows(self, agent: str | None = None) -> list[dict]:
        return self.call(f"windows {self._a(agent)}")["windows"]

    def stats(self, agent: str | None = None, reset: bool = False) -> dict:
        return self.call(f"stats {self._a(agent)}{' reset' if reset else ''}")

    def run(self, ms: int, agent: str | None = None) -> dict:
        return self.call(f"run {self._a(agent)} {ms}")

    def gate(self, agent: str | None = None) -> dict:
        return self.call(f"gate {self._a(agent)}")

    # ---- 坐标换算 ----
    def window_to_screen(self, pid: int, x: float, y: float, agent: str | None = None,
                         origin: str = "geometry") -> tuple[float, float]:
        """把窗口内坐标（例如 Wayland 下 AT-SPI 给出的坐标）换算成输出坐标。

        origin='geometry' 以窗口几何框为原点，'surface' 以表面左上角为原点；
        AT-SPI 在各工具包下用哪个原点待确认，两种都提供。
        """
        wins = [w for w in self.windows(agent) if w["pid"] == pid]
        if not wins:
            raise HeddleError(f"no window for pid {pid}")
        w = next((w for w in wins if w["focused"]), wins[0])
        if origin == "surface":
            return w["surface_x"] + x, w["surface_y"] + y
        return w["x"] + x, w["y"] + y
