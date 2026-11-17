# Heddle 文档

这些文档从 SandWeaver 的 [研究计划](https://github.com/aeilot/SandWeaver/blob/main/docs/research.md)中摘出与 Heddle 相关的部分，按主题重新组织。原文的核对标注保留：**全文**为读过正文相关章节，**摘要**为只读过摘要或项目页，**文档**为官方文档或项目页，**二手**为第三方评述，**未复核**为依据已知文献、没有重新打开原文。

| 文档 | 内容 |
|---|---|
| [设计依据](design.md) | 为什么要一个面向 agent 的桌面：已测的浪费、对 agent 无用的机制、"不需要显示"的分级、无头合成器与按观测出帧 |
| [隔离与共享](isolation.md) | 多 agent 共用一个合成器、应用状态隔离、XDG 与 D-Bus、内存共享 |
| [研究方法](method.md) | 移植已有方案再找新问题的方法、候选方案表、负载选择 |
| [门槛实验](gate.md) | Heddle always 对照 observe 的实测结果与暂定结论 |
| [内存曲线](memory.md) | N 个 agent 下共享合成器、共享 /usr 各省多少；内存上限下的缺页、读盘与步耗时 |

除标为"已测"的条目外，均为分析或设计提案，未经验证。实测数据来自 SandWeaver 的 [characterization](https://github.com/aeilot/SandWeaver/blob/main/docs/characterization.md)。
