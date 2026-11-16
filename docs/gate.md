# 门槛实验：Heddle always 对照 observe（2026-11-16，已测）

问题：按观测出帧省下的 CPU 有多大？动作后的余波里，渲染占多少，应用逻辑占多少？结论决定第 3 步是否值得继续做，还是把重心转到内存（见 [README](../README.md#门槛先确认按观测出帧值得做)）。

原计划用 sway 关输出来做这一步，但 sway 1.10.1 无头加 pixman 下，再打开输出后截图会卡住，所以改用 Heddle 自己的两种策略对照。

## 设置

- 环境：OrbStack 开发容器，Debian trixie，arm64，wlroots 0.18，pixman 软件渲染，1920x1080，所有应用以 root 运行（Chromium 因此加 `--no-sandbox`）。
- 应用：arm64 上能装到的 OSWorld 同类应用。Chromium 打开一个本地表单页（自动聚焦的输入框加长文，无网络、无动画）；GNOME Text Editor（GTK4）；LibreOffice Writer（gtk3 插件）。三者都以纯 Wayland 运行。
- 循环：预热 20 s；每步做一个动作（敲一句话、Tab、翻页、回车、Ctrl+Home），等 24 s（公开轨迹中步间隔的中位数），然后截图。每个应用每种策略 6 步，各跑 1 次。
- 策略：`always`（普通合成器）；`observe`（动作后放行 2 s，截图时放行到画面稳定，settle 300 ms）；`observe` 加 xdg_toplevel `suspended`。
- 指标：整步平均核数（动作、等待、截图都算，因为 observe 把渲染推到了截图时）；等待期按秒采样的 CPU；每步合成帧数；截图延迟和稳定/超时；旧画面（截图与随后一张 settle 2 s 的参考图相差超过 4096 像素即算旧画面，用来排除光标闪烁）；结束时的 PSS。
- 复现：`docker build -t heddle-apps -f dev/Dockerfile.apps dev`，再 `docker run --rm --privileged -v "$PWD":/src heddle-apps bench/gate.sh 6 24 1`。

## 结果

| 应用 | 策略 | 应用整步核数 | 等待期 0–2 s | 2–10 s | 10–24 s | 合成帧/步 | 截图延迟 | 旧画面 | 应用 PSS |
|---|---|---|---|---|---|---|---|---|---|
| Chromium | always | 0.021 ± 0.010 | 0.048 | 0.017 | 0.020 | 42 | 309 ms | 0/6 | 293 MiB |
| Chromium | observe | 0.020 ± 0.011 | 0.065 | 0.016 | 0.020 | 16 | 314 ms | 0/6 | 313 MiB |
| Chromium | observe + suspended | 0.019 ± 0.011 | 0.055 | 0.016 | 0.018 | 15 | 314 ms | 0/6 | 319 MiB |
| Text Editor | always | 0.032 ± 0.024 | 0.030 | 0.084 | 0.005 | 188 | 304 ms | 0/6 | 156 MiB |
| Text Editor | observe | 0.005 ± 0.003 | 0.025 | 0.012 | 0.000 | 28 | 303 ms | 0/6 | 129 MiB |
| Text Editor | observe + suspended | 0.005 ± 0.003 | 0.030 | 0.011 | 0.000 | 28 | 310 ms | 0/6 | 128 MiB |
| Writer | always | 0.011 ± 0.001 | 0.048 | 0.009 | 0.010 | 43 | 311 ms | 0/6 | 215 MiB |
| Writer | observe | 0.003 ± 0.001 | 0.041 | 0.002 | 0.001 | 8 | 313 ms | 0/6 | 270 MiB |
| Writer | observe + suspended | 0.003 ± 0.001 | 0.046 | 0.001 | 0.001 | 8 | 314 ms | 0/6 | 216 MiB |

合成器自身在所有组合下都约 0.002–0.003 核，PSS 27–36 MiB。54 次截图全部判为稳定，没有超时，没有旧画面；截图延迟约 300–315 ms，基本就是 settle 的 300 ms。

## 解读

- **Chromium：余波几乎全是非渲染开销。** observe 把合成帧从 42 降到 16，CPU 却没变（0.020 核）。等待后段（10–24 s）没有任何帧，仍稳定在约 0.02 核，所以这部分来自 Chromium 自身的后台活动，与是否出帧无关。
- **GTK4 与 LibreOffice：余波以渲染为主，但绝对量很小。** Text Editor 降 84%（0.032 → 0.005），Writer 降 70%（0.011 → 0.003）。Text Editor 在 always 下 2–10 s 的 0.084 核是光标闪烁，约 10 s 后 GTK 自己停止闪烁；Writer 的光标一直闪，后段持续约 0.01 核。observe 下这些都消失了。
- **绝对节省很小。** 每个应用每步最多省约 0.027 核。同一批应用占 130–320 MiB PSS。按 [设计依据](design.md#问题已测) 中"内存先于 CPU 耗尽"的估算，这么小的 CPU 节省改变不了密度。
- **`suspended` 没有可测的额外效果。** 三个应用在停帧时已经不画了，再告诉它们"不可见"也没有更多节流。
- **按观测出帧本身没有造成损害**：没有卡住的应用，没有旧画面，截图延迟只多出 settle 时间。

## 结论（暂定）

就这组数据，按 README 的判据：Electron 类应用（Chromium 是 VS Code 的近似）的余波以应用逻辑为主、渲染占比接近零；GTK/LibreOffice 的渲染占比高，但总量只有百分之一核量级。**支持把第 3 步降为可选、重心转到内存。** 第 3 步已实现的 observe 策略可以保留：它无害，还顺带消掉了光标闪烁。

不能据此下定论的地方：

- 负载很轻：本地静态页面、短文本输入，没有页面加载、语言服务器、文件索引。characterization 里虚拟机的余波是 0.3–1.2 核，vs_code 等待期稳定约 0.8 核，比这里高一到两个数量级，这部分差距从哪来还没解释。
- 没有测 VS Code，也没有 OSWorld 的真实任务；arm64 容器跑在 macOS 虚拟机里，CPU 数字受宿主噪声影响。
- 每组只跑 1 次、6 步；PSS 在同一应用的不同运行之间相差可达 55 MiB（Writer 215 对 270），内存数字不能用来比较策略。
- 只用了 pixman 软件渲染，没有 GPU 路径。

下一步建议：在 x86 主机上用 VS Code 和带页面加载的 Chromium 任务重复这组对照，并按进程拆分 vs_code 那 0.8 核的来源（渲染进程、扩展宿主、语言服务器）。
