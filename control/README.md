# 控制端

计划第 0 步：在 Wayland 下替代 OSWorld 虚拟机内基于 pyautogui 和 X11 截图的控制服务。截图和输入都走合成器的控制套接字（协议见 [compositor](../compositor/README.md#控制协议)），不经 screencopy 或虚拟输入协议。只用 Python 标准库。

```bash
export PYTHONPATH=$PWD/control
```

```bash
python3 -m heddle --control $XDG_RUNTIME_DIR/heddle/control -a a0 screenshot /tmp/a0.png
```

```bash
python3 -m heddle -a a0 pyautogui 'pyautogui.click(200, 300); pyautogui.typewrite("hello")'
```

```bash
python3 -m heddle -a a0 serve --port 5000
```

| 模块 | 内容 |
|---|---|
| `heddle.client.Heddle` | 客户端库；`window_to_screen(pid, x, y)` 用合成器导出的窗口位置把窗口内坐标（Wayland 下 AT-SPI 给的）换算成输出坐标 |
| `heddle.pyautogui_shim` | pyautogui 替身，覆盖 OSWorld 动作里常用的点击、拖动、滚动、敲字、热键；未覆盖的函数抛 `NotImplementedError`，便于回放时统计 |
| `heddle.server` | OSWorld 风格 HTTP 服务的最小子集：`GET /screenshot`、`POST /execute`、`GET /windows`、`GET /stats`。接口名待对照 OSWorld 源码核对 |

待做：

- AT-SPI 控件树接口（`/accessibility`）：需要在 agent 的 bwrap 内连到该 agent 的无障碍总线。
- AT-SPI 坐标的原点是窗口几何框还是表面左上角，各工具包待实测；`windows` 两个都给。
