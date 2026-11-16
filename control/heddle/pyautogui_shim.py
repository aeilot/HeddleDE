"""pyautogui 的替身：把 OSWorld 动作脚本里的 pyautogui 调用转成 Heddle 控制命令。

OSWorld 的动作是形如 `pyautogui.click(100, 200)` 的 Python 片段。用法：

    from heddle import pyautogui_shim
    pyautogui_shim.bind(Heddle(agent="a0"))
    exec(action, {"pyautogui": pyautogui_shim, "time": time})

只覆盖常用函数；没覆盖的函数抛 NotImplementedError，便于在回放中统计。
"""
from __future__ import annotations

import time as _time

from .client import Heddle

_h: Heddle | None = None
_pos = [0.0, 0.0]
FAILSAFE = False
PAUSE = 0.0

# pyautogui 键名 -> xkb 键名
_KEYMAP = {
    "enter": "Return", "return": "Return", "esc": "Escape", "escape": "Escape",
    "tab": "Tab", "space": "space", "backspace": "BackSpace", "delete": "Delete", "del": "Delete",
    "up": "Up", "down": "Down", "left": "Left", "right": "Right",
    "home": "Home", "end": "End", "pageup": "Prior", "pagedown": "Next", "pgup": "Prior", "pgdn": "Next",
    "insert": "Insert", "ctrl": "Control_L", "ctrlleft": "Control_L", "ctrlright": "Control_R",
    "shift": "Shift_L", "shiftleft": "Shift_L", "shiftright": "Shift_R",
    "alt": "Alt_L", "altleft": "Alt_L", "altright": "Alt_R",
    "win": "Super_L", "winleft": "Super_L", "super": "Super_L", "command": "Super_L", "cmd": "Super_L",
    "capslock": "Caps_Lock", "printscreen": "Print", "menu": "Menu", "apps": "Menu",
    "volumeup": "XF86AudioRaiseVolume", "volumedown": "XF86AudioLowerVolume", "volumemute": "XF86AudioMute",
    "-": "minus", "=": "equal", ",": "comma", ".": "period", "/": "slash", ";": "semicolon",
    "'": "apostrophe", "[": "bracketleft", "]": "bracketright", "\\": "backslash", "`": "grave",
    "+": "plus", "\n": "Return", "\t": "Tab", " ": "space",
}
for _i in range(1, 25):
    _KEYMAP[f"f{_i}"] = f"F{_i}"


def bind(h: Heddle) -> None:
    global _h
    _h = h


def _c() -> Heddle:
    if _h is None:
        raise RuntimeError("pyautogui_shim.bind() not called")
    return _h


def _key(name: str) -> str:
    return _KEYMAP.get(name.lower(), name) if len(name) > 1 else _KEYMAP.get(name, name)


def _xy(x, y):
    if x is None and y is None:
        return _pos[0], _pos[1]
    if isinstance(x, (tuple, list)):
        return float(x[0]), float(x[1])
    return float(x if x is not None else _pos[0]), float(y if y is not None else _pos[1])


def size():
    a = _c().agents()
    for ag in a["agents"]:
        if ag["name"] == _c().agent:
            return ag["width"], ag["height"]
    return 1920, 1080


def position():
    return int(_pos[0]), int(_pos[1])


def moveTo(x=None, y=None, duration=0.0, *args, **kw):
    x, y = _xy(x, y)
    _pos[:] = [x, y]
    _c().move(x, y)


def moveRel(dx=0, dy=0, *args, **kw):
    moveTo(_pos[0] + (dx or 0), _pos[1] + (dy or 0))


move = moveRel


def click(x=None, y=None, clicks=1, interval=0.0, button="left", *args, **kw):
    x, y = _xy(x, y)
    _pos[:] = [x, y]
    _c().click(x, y, button=button, count=int(clicks))


def doubleClick(x=None, y=None, *args, button="left", **kw):
    click(x, y, clicks=2, button=button)


def tripleClick(x=None, y=None, *args, button="left", **kw):
    click(x, y, clicks=3, button=button)


def rightClick(x=None, y=None, *args, **kw):
    click(x, y, button="right")


def middleClick(x=None, y=None, *args, **kw):
    click(x, y, button="middle")


def mouseDown(x=None, y=None, button="left", *args, **kw):
    if x is not None or y is not None:
        moveTo(x, y)
    _c().button(button, True)


def mouseUp(x=None, y=None, button="left", *args, **kw):
    if x is not None or y is not None:
        moveTo(x, y)
    _c().button(button, False)


def dragTo(x=None, y=None, duration=0.0, button="left", *args, **kw):
    mouseDown(button=button)
    tx, ty = _xy(x, y)
    sx, sy = _pos
    steps = 10
    for i in range(1, steps + 1):
        moveTo(sx + (tx - sx) * i / steps, sy + (ty - sy) * i / steps)
    mouseUp(button=button)


def dragRel(dx=0, dy=0, *args, **kw):
    dragTo(_pos[0] + dx, _pos[1] + dy, *args, **kw)


def scroll(clicks, x=None, y=None, *args, **kw):
    if x is not None or y is not None:
        moveTo(x, y)
    # pyautogui：正数向上；Wayland 轴：正数向下
    _c().scroll(0, -int(clicks))


vscroll = scroll


def hscroll(clicks, x=None, y=None, *args, **kw):
    if x is not None or y is not None:
        moveTo(x, y)
    _c().scroll(int(clicks), 0)


def typewrite(message, interval=0.0, *args, **kw):
    if isinstance(message, (list, tuple)):
        for k in message:
            press(k)
        return
    _c().type(str(message))


write = typewrite


def press(keys, presses=1, interval=0.0, *args, **kw):
    if isinstance(keys, str):
        keys = [keys]
    for _ in range(int(presses)):
        for k in keys:
            if len(k) == 1 and k not in _KEYMAP:
                _c().type(k)
            else:
                _c().key(_key(k))
            if interval:
                _time.sleep(interval)


def keyDown(key, *args, **kw):
    _c().keydown(_key(key))


def keyUp(key, *args, **kw):
    _c().keyup(_key(key))


def hotkey(*keys, **kw):
    _c().key("+".join(_key(k) for k in keys))


def sleep(s):
    _time.sleep(s)


def screenshot(*args, **kw):
    raise NotImplementedError("use Heddle.screenshot()")


def __getattr__(name):
    raise NotImplementedError(f"pyautogui.{name} is not supported by heddle shim")
