# heddle 合成器

计划第 2、3 步的原型：基于 wlroots 0.18 的无头合成器，单个 C 文件。起点按 tinywl 的结构自写（"待定"中三选一的结果，理由见下）。

## 构建与运行

需要 Linux。macOS 上用仓库里的开发容器：

```bash
docker build -t heddle-dev dev
```

```bash
docker run --rm --privileged -v "$PWD":/src heddle-dev tests/smoke.sh
```

直接在 Linux 上：

```bash
meson setup build compositor && ninja -C build
```

```bash
build/heddle -a a0 -a a1 -s 1920x1080 -p observe -g 2000
```

| 选项 | 含义 |
|---|---|
| `-a NAME` | 加一个 agent，可重复；默认一个 `a0` |
| `-s WxH`、`-r HZ` | 输出尺寸与刷新率 |
| `-p always\|observe` | 出帧策略，见下 |
| `-g MS` | observe 下动作后的放行期，默认 2000 |
| `-S` | 停帧期间同时给窗口发 xdg_toplevel `suspended` |
| `-w` | 不最大化窗口，改为层叠 |
| `-R DIR` | 运行目录，默认 `$XDG_RUNTIME_DIR/heddle` |

运行目录布局：`DIR/<agent>/wayland-0` 是该 agent 的 Wayland 套接字（`WAYLAND_DISPLAY` 可直接设成这个绝对路径），`DIR/control` 是控制套接字，`DIR/ready` 在就绪后写入 pid。

## 结构

- 每个 agent 一个无头输出、一个 seat（名字即 agent 名）、一张独立的场景图、一个监听套接字。合成器自己 accept，因此知道每个客户端属于哪个 agent。
- 全局对象过滤（`wl_display_set_global_filter`）：seat 和输出只对所属 agent 的客户端可见。应用看到的是"单 seat、单输出"的普通桌面，不用依赖工具包的多 seat 支持。
- 不提供 screencopy、虚拟键盘、虚拟指针、data-control：截图和输入只走控制套接字。
- 剪贴板与主选区按 seat 分开。
- 渲染固定为 pixman，客户端只能用 wl_shm。
- 没有 Xwayland（见 README"待定"）。

为什么不从 cage 或 labwc 起步：cage 是单应用 kiosk，多输出多 seat 要大改；labwc 带完整的人用窗口管理（菜单、主题、配置），删比加多。需要的协议在 wlroots 里大多一行创建，tinywl 的结构更容易插入按 agent 的过滤和出帧控制。缺的协议（输入法、xdg-decoration、xdg-output、fractional-scale、cursor-shape 等）按应用需要再加。

## 控制协议

Unix 流套接字，每行一条命令（空格分隔），合成器回一行 JSON（`{"ok":true,...}` 或 `{"ok":false,"error":...}`）。同一连接上的命令按顺序执行；截图会挂起该连接直到完成。

| 命令 | 作用 |
|---|---|
| `agents` | 列出 agent、套接字、输出与当前策略 |
| `screenshot A PATH [settle=300] [timeout=5000] [threshold=4096]` | 放行帧，等画面稳定后写 PNG。回复 `status`（`stable` 或 `timeout`）、`latency_ms`、本次合成帧数、光标位置 |
| `move A X Y` | 移动指针（输出坐标） |
| `click A X Y [button=left\|right\|middle] [count=N]` | 移动并点击；按下时聚焦所点窗口 |
| `button A B down\|up` | 单独按下或松开 |
| `scroll A DX DY` | 滚轮格数，正数向下/向右 |
| `key A COMBO` | 组合键，如 `ctrl+shift+t`、`Return`；键名为 xkb keysym 名，另认 ctrl/shift/alt/super/enter/esc 等别名 |
| `keydown A K`、`keyup A K` | 单独按下或松开 |
| `type A TEXT` | 行内剩余部分作为文本逐字符敲入，支持 `\n` `\t` `\\`；返回键位表中缺失的字符数 `missing` |
| `windows A` | 窗口列表：`id`、`pid`、`app_id`、`title`、几何框 `x/y/width/height`、表面原点 `surface_x/y`、`focused` |
| `stats A [reset]` | 帧与提交统计：已合成帧、被跳过的帧事件、发出的帧回调、表面提交数与损坏像素，按客户端 pid 细分 |
| `run A MS`、`gate A` | 手动放行 MS 毫秒；立即停帧 |

Python 封装见 [control](../control)。

## 出帧策略

`always`：普通合成器的行为，有损坏就合成并发帧回调。

`observe`（第 3 步）：

1. 只有在"放行"状态下才合成、才发 `wl_surface.frame` 回调。停帧时帧事件直接丢弃并计入 `frames_skipped`，客户端拿不到回调就不再重画。
2. 每个输入动作把放行期延长到 `now + grace`；启动时也放行一个 grace，让应用画出首帧。
3. 截图请求放行帧时钟，每 20 ms 检查一次：自上次"有效损坏"（单次提交损坏面积大于 `threshold` 像素，用来忽略光标闪烁）起已过 `settle` 毫秒、且没有待合成的损坏，即判为稳定；超过 `timeout` 则按超时返回当前帧。
4. 截图完成后，若不在放行期内，立即停帧。
5. `-S` 时，停帧同时给窗口发 `suspended`，放行时撤回。

已知局限（与 README"已知风险"对应）：

- 停帧期间仍照常向客户端发输入和配置事件；被冻结（cgroup freezer）的客户端不读套接字，会写满缓冲区，此原型没有处理。
- 稳定判定只看表面提交的缓冲损坏，不看子表面以外的状态变化（例如无损坏的窗口移动）。
- `type` 只能敲出当前键位表（us）里有的字符；中文等需要像 wtype 那样临时上传键位表，或接输入法协议。
