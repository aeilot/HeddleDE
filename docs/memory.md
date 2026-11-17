# 内存曲线：共享合成器与共享 /usr（2026-11-16，已测）

问题：N 个 agent 同时运行时，总内存怎么增长？Heddle 设计里的两种共享各省多少：

- 多个 agent 共用一个合成器（对照：每个 agent 一个独立合成器）。[隔离与共享](isolation.md) 估计这部分只省几十 MiB（未测）。
- 所有 agent 共用只读 `/usr`，文件页只存一份（对照：每个 agent 一份 `/usr` 拷贝，近似每个 agent 一个独立镜像）。

这是 README 计划第 4 步的第一组数据，也是 [门槛实验](gate.md) 把重心转到内存之后的第一步。

## 设置

- 环境：与门槛实验相同（OrbStack 开发容器，Debian trixie，arm64，12 GiB 内存，wlroots 0.18，pixman，1920x1080，root 运行）。全程没有用到交换区（SwapPss 为 0）。
- 应用：Chromium（本地表单页，`--no-sandbox`）、GNOME Text Editor、LibreOffice Writer。每个 agent 一个应用实例，全部经 `heddle-run` 启动在各自的 bubblewrap 里（独立 HOME、/tmp、D-Bus 会话，默认断网），出帧策略 `observe`。
- 配置：
  - `shared`：一个 Heddle 进程服务全部 agent，所有沙盒绑定宿主 `/usr`。
  - `per-comp`：每个 agent 一个 Heddle 进程，仍共用 `/usr`。
  - `per-usr`：一个 Heddle 进程，每个沙盒绑定自己的一份 `/usr` 拷贝（`cp -a`，在 overlay 上是不同的 inode，因此页缓存不共享）。合成器本身仍用宿主 `/usr`。
- 流程：每个 N ∈ {1, 2, 4, 8} 依次启动 N 个应用（间隔 1 s），等全部出窗口，再等 15 s；每个 agent 敲两行字并截图；再等 10 s 后采样。每组只跑 1 次。
- 指标：合成器与各沙盒内全部进程（bwrap、dbus-run-session、dbus-daemon、AT-SPI、应用）的 PSS 之和。PSS 把共享页按映射它的进程数分摊，所以总和就是这些进程实际占用的常驻内存，并按匿名页、文件页、共享内存（主要是 wl_shm 窗口缓冲）拆分。不含内核内存（页表、slab、套接字缓冲）和未映射的页缓存。
- 复现：`docker run --rm --privileged -v "$PWD":/src -w /src heddle-apps bench/memcurve.sh "1 2 4 8" 1`。`per-usr` 需要 N 份 `/usr` 拷贝，每份约 2 GiB 磁盘。

所有 36 组都出了窗口、截图判为稳定。

## 结果

总 PSS（MiB）：

| 应用 | 配置 | N=1 | N=2 | N=4 | N=8 | 每多一个 agent |
|---|---|---|---|---|---|---|
| Chromium | shared | 439 | 654 | 1054 | 1809 | 196 |
| Chromium | per-comp | 441 | 646 | 1049 | 1802 | 194 |
| Chromium | per-usr | 444 | 888 | 1756 | 3448 | 429 |
| Text Editor | shared | 259 | 422 | 736 | 1354 | 156 |
| Text Editor | per-comp | 259 | 414 | 727 | 1345 | 155 |
| Text Editor | per-usr | 264 | 530 | 1051 | 2082 | 260 |
| Writer | shared | 305 | 479 | 809 | 1476 | 167 |
| Writer | per-comp | 305 | 472 | 803 | 1468 | 166 |
| Writer | per-usr | 310 | 616 | 1220 | 2430 | 303 |

"每多一个 agent"是 N=1 到 N=8 的斜率。N=8 时的拆分（MiB）：

| 应用 | 配置 | 匿名页 | 文件页 | 共享内存 | 其中合成器 PSS（不含共享内存） |
|---|---|---|---|---|---|
| Chromium | shared | 1214 | 248 | 347 | 212（22） |
| Chromium | per-comp | 1207 | 248 | 347 | 205（16） |
| Chromium | per-usr | 1215 | 1886 | 347 | 215（25） |
| Text Editor | shared | 991 | 110 | 253 | 211（21） |
| Text Editor | per-comp | 982 | 110 | 253 | 205（16） |
| Text Editor | per-usr | 993 | 836 | 253 | 215（25） |
| Writer | shared | 1143 | 141 | 192 | 149（22） |
| Writer | per-comp | 1136 | 140 | 192 | 143（16） |
| Writer | per-usr | 1143 | 1095 | 192 | 151（25） |

## 解读

- **共享 `/usr` 是主要的节省。** 共享时文件页几乎不随 N 增长（Chromium 236 → 248 MiB），不共享时线性增长（每个 agent 约 105–236 MiB）。N=8 时总内存少 35–48%：Chromium 少 1.6 GiB，Text Editor 少 0.7 GiB，Writer 少 0.9 GiB。每多一个 agent 的成本从 260–429 MiB 降到 156–196 MiB。
- **共享合成器在内存上没有可测的收益。** `per-comp` 反而比 `shared` 小 0–9 MiB，在单次运行的噪声之内。原因是 N 个 Heddle 进程的代码和库本身就通过页缓存共享，每个进程自己的匿名内存只有约 2 MiB；而按 agent 计的大头（窗口缓冲、输出缓冲）不论合成器是否共享都一样多。[隔离与共享](isolation.md) 估计的"几十 MiB"偏高，实测接近零。注意这里的 `per-comp` 仍共用 `/usr`；如果每个合成器来自不同的镜像，每个还要多几 MiB 文件页。
- **剩下的边际成本几乎全是匿名页。** 共享 `/usr` 后，每个 agent 的边际成本里约 75–85% 是应用自己的匿名内存（堆、JS 引擎、渲染缓存），其余主要是 1080p 窗口缓冲（每块约 8 MiB，应用和合成器各持有几块，合计每个 agent 24–43 MiB）。
- **重复的文件页是"可回收"的。** 三种配置下 `MemAvailable` 的下降几乎相同（Chromium N=8 都约 1.8 GiB），因为内核把干净的文件页算作可回收。也就是说，不共享 `/usr` 时多出的 0.7–1.6 GiB 在内存紧张时可以被换出，代价是缺页时从磁盘重新读入。硬性的上限由匿名页和共享内存决定，这部分三种配置一样。共享 `/usr` 的实际收益要在内存压力下测：同样的内存上限下，不共享时会更早开始频繁缺页、截图变慢。

## 结论（暂定）

- 保留共享只读 `/usr`：它在常驻内存上省 35–48%，并在内存紧张时避免重复的代码页互相挤出。
- 共享合成器不再以内存为理由。它和"每 agent 一个合成器"在内存上打平，是否共享应按其他因素决定：共享合成器是所有 agent 的可信计算基，崩溃会影响全部 agent（README"已知风险"）；每 agent 一个合成器隔离更好，但多出 N 个进程和控制端点。就这组数据，每 agent 一个合成器是可行的默认值。
- 下一个内存杠杆是匿名页，按 [隔离与共享](isolation.md#内存共享) 的候选：先用 KSM 统计 N 个同类实例匿名页中内容相同的比例；其次是窗口缓冲（降低分辨率，或在停帧期间释放输出缓冲）。

不能据此下定论的地方：

- 每组只跑 1 次，应用基本空闲（打开一个页面或空文档、敲两行字）。真实任务中打开大文档、加载网页、运行扩展后匿名页会大得多，共享 `/usr` 的相对比例会下降。
- 没有测 VS Code。Chromium 只是近似，Electron 应用另带 Node 进程和扩展宿主。
- 没有在内存压力下测（cgroup 内存上限 + 缺页次数和截图延迟），所以"可回收的重复页"的真实代价还没量化。
- PSS 不含内核内存。N 个沙盒各有自己的 dbus-daemon 和 AT-SPI 进程，合计每个 agent 约 1–2 MiB，已计入总数。
