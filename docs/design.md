# 设计依据

## 问题（已测）

交互式桌面 agent 的桌面虚拟机约 3.5 GiB 内存，大部分时间在等模型（见 SandWeaver 的 [characterization](https://github.com/aeilot/SandWeaver/blob/main/docs/characterization.md#第二期交互式云桌面2026-11-15-启动)）。

- 内存：约一半是客户机页缓存，约三成是进程内存。
- CPU：9 个应用域的试点中，多数应用在动作后约 10 s 内有 0.3–1.2 核的余波，之后降到 0.03–0.18 核；vs_code 整个等待期稳定在约 0.8 核。按等待时长加权，平均约 0.31 核。
- 节奏：公开轨迹中步间隔中位数 24 s，每步截图 0.5–1.6 MB。

由此得出的约束（估算，未实测）：在常见的每核 4 GiB 宿主上，以每台 3.6 GiB、等待期 0.31 核计，内存先于 CPU 耗尽，大约在 CPU 用到三成时内存就满了。单独节省等待期 CPU 不能提高密度，除非同时解决内存。这是 Heddle 计划中"门槛"一步的来由。

## 对 agent 无用的桌面机制（分析，未验证）

人用桌面的许多机制假设有人坐在屏幕前。内存数字来自 libreoffice_calc 的单次快照（RSS 含共享页），其余为推测。

| 类别 | 机制 | 依据 |
|---|---|---|
| 无用且有成本（去掉） | 软件中心、更新与固件服务（snap-store、snapd、snapd-desktop-integration、packagekitd、fwupd）；日历与邮件提醒（evolution-alarm）；文件索引、崩溃上报、定位、色彩管理、蓝牙、打印；登录管理器与多用户会话切换；合成器动画与光标闪烁 | 前五个服务 RSS 合计约 420 MiB；空闲期 top 中 evolution-alarm 偶尔排第一；动画与闪烁的开销未拆分 |
| 对 agent 有害 | 锁屏、屏保与显示器休眠（模型等待中位 24 s）；通知弹窗遮挡点击位置；更新提示、首次运行向导、授权与密钥环弹窗 | 推测，需要从失败轨迹中核实 |
| 形式不对 | 物理显示、GPU、DRM/KMS 与垂直同步（agent 只需观测时的一帧，可软件渲染）；内核输入设备与 udev 热插拔；声卡（空输出即可）。无障碍树相反，应保留并加强 | 设计判断 |
| 虚拟化层重复 | 每台一份客户机内核、固件与完整 systemd 启动；客户机页缓存约 1.7 GiB；已触及的空闲页 0.5–0.8 GiB；QEMU 模拟的 VGA、USB 平板、声卡 | 内存来自快照 |

快照中与显示相关的进程：gnome-shell 399 MiB、Xorg 180 MiB、xdg-desktop-portal 124 MiB、gjs 111 MiB，按 RSS 合计约 0.8 GiB；任务应用本身 soffice.bin 为 279 MiB。按快照估算，3.5 GiB 中约 2.5 GiB 属于第一类和第四类，应用本身、显示服务器和 agent 服务约 0.3–0.6 GiB（未实测）。

去掉人用设备后，桌面不再需要 `/dev/dri`、`/dev/input`、`/dev/tty`、`/dev/snd`。共享内核方案此前的障碍是设备命名空间没有进入主线（[2013 RFC](https://lwn.net/Articles/564854)，[2021 讨论](https://lore.kernel.org/containers/877dj4ff9g.fsf@disp2133/)），这时它不再必需，剩下的只是普通系统调用、共享内存和 Unix 套接字。安卓的 ReDroid、[Anbox Cloud](https://documentation.ubuntu.com/anbox-cloud/) 已在共享内核上大规模运行 GUI 实例。

## "不需要显示"的分级（2026-11-15，未验证）

| 级别 | agent 看到什么 | 可以去掉什么 | 已有工作或证据 |
|---|---|---|---|
| L0 无头但持续渲染（现状） | 截图 | 物理显示器 | Xvfb、VNC、OSWorld 虚拟机 |
| L1 按观测渲染 | 截图，只在 agent 要观测时合成 | 观测之间的合成与重绘：动画、光标闪烁、视频 | Wayland 允许合成器不给被遮挡窗口发帧回调，[KWin](https://bugs.kde.org/show_bug.cgi?id=464217)、Mutter、Chromium、GTK、Qt 已有相应行为；[waydriver](https://docs.rs/crate/waydriver/latest) 注明无头 Wayland 下 GTK4 只画首帧，但仍能读 AT-SPI 树。没有找到按 agent 观测驱动帧时钟的系统 |
| L2 只要语义 | 无障碍树，按元素操作 | 合成、渲染和截图通路；应用仍需布局 | OSWorld 原论文中 GPT-4 只用无障碍树成功率 12.2%，高于只用截图的约 5.3%（二手，2024 年模型）；部分工具包只有窗口被映射后才发布无障碍树 |
| L3 不要 GUI | 应用 API | 整个桌面 | [UFO2](https://arxiv.org/abs/2504.14603)、[API Agents vs. GUI Agents](https://arxiv.org/abs/2503.11069)（ICML '25） |

Heddle 瞄准 L1，同时保留 L2 的接口。当前模型的趋势是直接看截图，L2 的成功率优势来自 2024 年的模型，需要重新确认。

SandWeaver 对 OSWorld 360 个任务的分析显示，约 97% 存在不经桌面的做法（分析脚本在 SandWeaver 本地工作区，尚未入库）。这只说明 OSWorld 因为必须能脚本判分而偏向文件终态，不能说明真实负载中必须用 GUI 的比例低。论证桌面方向需要真实负载中 GUI 必要任务的比例，这一数据目前没有。

## 面向 agent 的无头合成器（2026-11-16，设计提案）

在 Wayland 下，合成器负责管理窗口、拼合各应用的缓冲区、把输入分给对应的应用；应用彼此看不到对方的画面，截图和输入注入都要经过合成器。因此它是做按 agent 截图、输入、隔离和渲染控制的天然位置。

- 不以 Xfce 为底座：xfwm4 只支持 X11；Xfce 的 Wayland 合成器 [xfwl4](https://docs.xfce.org/xfce/xfwl4/start) 基于 Smithay（Rust），官方仍标为实验性（[路线图](https://wiki.xfce.org/releng/wayland_roadmap)）。面板、桌面、通知、电源管理对 agent 无用，所以"新桌面"实质上是一个新合成器加会话与设置管理。
- 只实现应用需要的协议（xdg-shell、wl_shm、wl_seat、剪贴板、输入法），不接 DRM/KMS 和 libinput；控件树走 AT-SPI，与合成器无关。底层库已选 wlroots，理由见 [README](../README.md#已定)。
- 按观测出帧：把"按下次观测截止期调度、截图前判定画面稳定"和"不可见就不渲染"合成一个机制。协议讨论建议不可见表面不发回调（[wayland#266](https://gitlab.freedesktop.org/wayland/wayland/-/issues/266)）；Mutter 只对被录屏的隐藏表面破例发回调（[MR 2789](https://gitlab.gnome.org/GNOME/mutter/-/merge_requests/2789)）。没找到把"只在观测时出帧"作为默认策略的合成器。具体策略见 [README](../README.md#按观测出帧的策略)。
- 已知风险：拿不到回调的 Firefox 和 GDK 会卡住（[Mozilla 1711244](https://bugzilla.mozilla.org/show_bug.cgi?id=1711244)，[Weston EGL](https://gitlab.freedesktop.org/wayland/weston/-/issues/597)）；光标和拖放表面从不被绘制，回调若绑定绘制就永远不发（[GNOME 739163](https://bugzilla.gnome.org/show_bug.cgi?id=739163)）；Xwayland 应用和自带定时器的视频、动画不受帧回调约束；截图要等应用重画，用延迟换 CPU。
- 前提待测：动作后约 10 s 的余波里，渲染占多少、应用逻辑（页面加载、排版、语言服务器）占多少。若以应用逻辑为主，压帧收益有限。

## 相关的"没人看却仍在运行"方案（2026-11-16）

| 方案 | 做法 | 已有工作 | 空白与风险 |
|---|---|---|---|
| A. 按下次观测截止期调度 | 等待期调低 `cpu.weight` 或 `cpu.max`，让余波慢慢完成；截图前确认已静止 | [Resource Deflation](https://lass.cs.umass.edu/papers/pdf/eurosys19-deflation.pdf)（EuroSys '19，摘要）；[Citrix WEM](https://docs.citrix.com/en-us/workspace-environment-management/service/using-environment-management/system-optimization/multi-session-optimization) 在用户断开后降优先级（文档）；[AgentCgroup](https://arxiv.org/abs/2602.09345)（全文） | 没找到以下次观测时间为截止期调度桌面的工作；新意只在策略。模型等待不可预知，需要静止检测兜底 |
| B. 静止后冻结 | `cgroup.freeze` 冻结桌面，下一个动作前恢复 | [Android cached apps freezer](https://source.android.com/docs/core/perf/cached-apps-freezer)（文档）；AWS Lambda 冻结执行环境；[LiteGreen](https://www.usenix.org/legacy/events/atc10/tech/full_papers/Das.pdf)（ATC '10） | 机制现成，每步粒度没人测过；静止后 CPU 省得少，价值在为内存回收创造前提。风险：时钟跳变、定时器积压、连接超时 |
| C. 让桌面知道"没人在看" | 观测之间标为不可见，让应用自己节流；截图时补画 | [Chrome 后台节流](https://developer.chrome.com/blog/timer-throttling-in-chrome-88)；[Weston MR 1524](https://gitlab.freedesktop.org/wayland/weston/-/merge_requests/1524)、[KWin 467032](https://bugs.kde.org/show_bug.cgi?id=467032) | 没找到按 agent 观测驱动帧时钟的系统；收益大小未知 |
| D. 精简常驻服务 | 去掉 snap-store、fwupd 等 | [CUA-Lite](https://rdi.berkeley.edu/blog/cua-lite)、[CUA-Sandbox](https://arxiv.org/abs/2609.32750)（摘要） | 工程，不是研究点 |

判断：四条机制都有先例。A 与 C 的组合是检索范围内没找到的部分，但贡献偏策略，CPU 收益也受上面的内存约束限制。Heddle 的按观测出帧即 A 加 C。
