# 隔离与共享

以下均为 2026-11-16 的讨论与检索，设计提案，未验证。

## 多个 agent 共用一个合成器

设想：一个合成器服务多个 agent，每个 agent 有自己的焦点、光标、剪贴板、可截图的输出和隔离的应用。

| 要分开的东西 | 做法 | 现有支持 |
|---|---|---|
| 焦点、光标、剪贴板 | Wayland 多 seat：每个 agent 一个 seat | wlroots 与 sway 已支持；[Musa](https://mainiomano.srht.site/blog/2024/03/introducing-musa/) 是专门的多 seat 合成器；SDL、Blender 等工具包对多焦点处理不完善（[综述](https://blinry.org/multi-seat-wayland/)） |
| 输入注入 | 合成器知道每个事件来自哪个 agent，按 seat 路由 | [libei](https://libinput.pages.freedesktop.org/libei/api/) 是现成方案；Heddle 计划由控制接口直接注入 |
| 截图 | 每个 agent 一个无头输出，只截自己的输出 | wlroots 已有无头输出与按输出截屏 |
| 应用隔离 | 每个 agent 的应用放进 bubblewrap：独立挂载、PID、网络命名空间，D-Bus 经过滤代理 | Flatpak 已用于单用户桌面 |
| 共享 | 内核、合成器、只读 `/usr` 的页缓存、字体与图标缓存 | 见下文"内存共享" |

为什么用 Wayland：X11 下任何客户端都能读其他窗口和全局输入，XTEST 注入是全局的，无法在一个 X 服务器里隔离多个 agent。

需要处理的冲突：

- 单实例应用：LibreOffice、Chrome、VS Code、Thunderbird 启动第二个实例时会把请求交给已有实例，跨 agent 串到一起。靠每个 agent 独立的 HOME、/tmp 和 D-Bus 打断。
- 修改全局状态的任务：OSWorld 的 os 域和部分设置类任务改的是系统或桌面设置，只能落到独立实例。
- 合成器是共享的可信计算基：它的漏洞就是跨 agent 漏洞。同一用户的多个 agent 可以接受，跨租户较弱。

相关工作：[Multi-Agent Computer Use](https://arxiv.org/abs/2606.01533) 给每个子 agent 一台虚拟机（摘要）；[agent-desktop](https://flaviocopes.com/agent-desktop/) 在 X11 上给每个 agent 画一个光标，但动作轮流执行（博客）；[Codex issue #20852](https://github.com/openai/codex/issues/20852) 请求并行的 computer-use 会话；Qubes 共享桌面但靠虚拟机隔离。

判断：

- 没找到用多 seat 合成器加应用沙盒、让多个 agent 并行共用一个桌面的系统。组件都已存在，新意在组合与冲突处理。
- 共享桌面环境省下的是合成器和外壳。每个 agent 改用最小合成器后，这部分估计只剩几十 MiB（未测），主要成本是应用本身。因此计划第 4 步要加"每个 agent 一个合成器"的对照。
- 共享桌面真正必要的场景是多个 agent 和用户本人在同一个桌面上并行工作。云上批量运行时，每个 agent 一个最小显示更简单。

## 应用状态隔离

不需要虚拟机，用 bubblewrap 给每个 agent 一套命名空间：

| 状态 | 不隔开的后果 | 办法 |
|---|---|---|
| 配置、扩展、缓存、最近文件 | 互相改设置 | 独立 HOME 与 XDG 目录（挂载命名空间） |
| 单实例锁与 IPC 套接字 | 两个 agent 串在一起 | 独立 `XDG_RUNTIME_DIR` 与 /tmp；VS Code 另有 `--user-data-dir` |
| D-Bus 会话总线 | 能找到并调用别的 agent 的应用 | 每个 agent 一条会话总线，或经 xdg-dbus-proxy 过滤 |
| 进程 | 能看到、杀掉、ptrace 别人的进程 | PID 命名空间 |
| 端口 | 语言服务器、调试器端口冲突或互连 | 网络命名空间 |
| 窗口、输入、剪贴板 | 互相截图、注入 | 合成器按 agent 分开 |
| 工作区 | 互相改代码 | 只挂载自己租用的工作区，沿用 SandWeaver 的独占租约 |
| CPU、内存 | 相互拖慢 | 每个 agent 一个 cgroup |

### 同时开 100 个 VS Code

- 内存很可能先到上限。Electron 每个窗口至少有主进程、渲染进程、GPU 进程和扩展宿主，再加语言服务器；按每个 0.5–1 GiB 估（未测），100 个需要 50–100 GiB。
- CPU：试点中 vs_code 域等待期稳定约 0.8 核，是整台虚拟机 cgroup 的数，没有拆到进程。若确实来自 VS Code，100 个约 80 核。可能来源有光标闪烁引起的重绘、文件监视、扩展宿主、语言服务器（假设）。若主要是重绘，按观测出帧能消掉；若是语言服务器，要靠冻结或 cgroup 限额。
- 编码任务大多不需要 VS Code 界面；真正需要大量可见 IDE 的只有以操作界面为目标的任务。

## XDG 与 D-Bus

[XDG Base Directory](https://specifications.freedesktop.org/basedir-spec/latest/) 规定应用把东西放在哪里：

| 变量 | 默认 | 内容 |
|---|---|---|
| `XDG_CONFIG_HOME` | ~/.config | 用户配置 |
| `XDG_DATA_HOME` | ~/.local/share | 用户数据、扩展 |
| `XDG_CACHE_HOME` | ~/.cache | 可删除的缓存 |
| `XDG_STATE_HOME` | ~/.local/state | 历史、最近文件 |
| `XDG_RUNTIME_DIR` | /run/user/UID（tmpfs） | Wayland、D-Bus、音频套接字，常见的单实例锁 |
| `XDG_CONFIG_DIRS`、`XDG_DATA_DIRS` | /etc/xdg、/usr/share | 系统级只读默认值 |

- 隔离：每个 agent 一套 `*_HOME` 与 `XDG_RUNTIME_DIR`。只读默认值放在 `XDG_*_DIRS`，所有 agent 共用一份。
- 局限：不是所有应用都遵守（VS Code 写 ~/.vscode，Firefox 写 ~/.mozilla，Thunderbird 写 ~/.thunderbird），所以还要用挂载命名空间给每个 agent 独立 HOME。
- [xdg-desktop-portal](https://flatpak.github.io/xdg-desktop-portal/) 是沙盒应用申请文件、截图等能力的受控接口，可作为 agent 的权限检查点。

[D-Bus](https://www.freedesktop.org/wiki/Software/dbus/) 是桌面上的进程间消息总线。会话总线每个登录会话一条，地址在 `DBUS_SESSION_BUS_ADDRESS`，通常是 `$XDG_RUNTIME_DIR/bus`。走会话总线的功能包括单实例转发、dconf/GSettings 设置、通知、MPRIS 媒体控制、Secret Service 密钥和门户。无障碍接口 AT-SPI 用一条单独的总线。

- 隔离：同一会话总线上任何应用都能调用其他应用，所以每个 agent 需要自己的总线，或经 xdg-dbus-proxy 只放行指定名字。无障碍总线也要每个 agent 一条。
- 用途：AT-SPI 与 MPRIS 是 agent 的语义接口，读控件树或控制播放比截图加点击更直接。

## 内存共享

- 文件页（代码、库、字体、图标、只读数据）：同一文件在宿主页缓存只有一份，所有进程自动共用。前提是共享内核，所有 agent 用同一只读底座即可。虚拟机下同样的代码要存 N 份，只能靠 virtio-fs DAX 或 KSM。
- 匿名页（堆、JS 堆、索引、渲染缓存）：默认私有。手段按预期收益排序：
  1. 快照克隆、写时复制：应用预热后快照，新实例以 `MAP_PRIVATE` 映射同一快照文件。Firecracker 快照与 Lambda SnapStart 是这样做的；容器下 CRIU 恢复是拷贝而不是映射，需要改造（未验证）。
  2. 模板进程 fork（安卓 zygote）：天然写时复制，但 Electron、Chrome 多线程，跨实例 fork 需要大改。
  3. KSM 合并相同页：Linux 6.4 起可用 `prctl(PR_SET_MEMORY_MERGE)` 对整个进程开启；代价是扫描 CPU 与侧信道风险。开启前需征得同意。
  4. 应用层共享（一个浏览器进程里多个隔离上下文）：省得最多，但隔离边界降到应用内部。
- 关键未知量：N 个实例的匿名页中内容相同的比例（KSM 统计可回答），以及运行后快照页被改写的比例。
