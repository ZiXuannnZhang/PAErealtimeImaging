# 环扫新链路重构 — 历史设计、崩溃分析与输入假设汇总

> **历史归档 / 非当前实现规范。**
>
> 本文件由原根目录 `环扫新链路重构报告.md`、`环扫新链路重构设计.md` 与 `补充信息.txt` 合并整理。三份原文包含大量中间方案与随后被实现修正的 API/SHM 设想；当前行为必须以 canonical main source 为准。原文仍可从 Git history 恢复。

## 1. 当时的问题背景

早期环扫实时成像曾反复出现 Qt `QList::at` fail-fast / 随机受害者崩溃。证据表明：

- 更换显示层后相同崩溃签名仍出现；
- 历史 gdb 的受害栈分散在 QCustomPlot、QPixmap 和 `ImagingController::frameDataToImage`；
- 因此当时判断显示控件更像受害者，主要风险位于重建结果搬运、共享内存、帧所有权与高频大帧回读链路。

原始 crash dump / log / backtrace / 调查报告已移至本目录 `crash-investigation/`；UI 重构过程截图位于 `screenshots/`。

## 2. 当时的负载结论

历史默认参数下，环扫 3600×3600 双波长全帧约 103.68 MB。每块均进行全分辨率回读/SHM 写入/接收端复制，导致相对于线性实例出现数量级更高的帧搬运和内存压力。

因此重构方向集中在：

- 明确 SHM magic/version/size；
- 减少跨线程隐式共享和运行期大对象分配；
- 固定缓冲 + 明确所有权；
- 将“显示快照”和“整圈完成”语义拆开；
- 圈边界显式 reset，避免跨圈 accumulator/residual 污染；
- PNG 与 UI presentation 的所有权/时序解耦。

后续 RoundIdentity、snapshot exact-seq、Ring hard barrier、TimeoutBoundary reset/capture 等实现已经显著演化，不能再按这份历史文档里的具体 API 名称推导当前行为。

## 3. 原设计演进中有价值的原则

原报告先提出“较小显示快照 + 固定双缓冲”；后续设计评估又选择过“全分辨率方案 A”。两者的具体尺寸方案已不具当前规范意义，但以下原则仍有历史价值：

1. SHM 尺寸/版本必须显式校验，陈旧映射不得静默复用；
2. reconstruction frame/presentation 需要单一、清晰的 ownership；
3. 运行期应避免高频 100MB 级临时分配；
4. 圈末 reset 与新轮 identity 必须形成 hard barrier；
5. display refresh 与 round completion 是不同语义；
6. 对历史崩溃不能只凭“显示开/关”推断根因，应结合 producer/consumer 时序和内存边界证据。

## 4. 原始输入假设 notes 中的独有信息

原 `补充信息.txt` 记录了当时用于实现/讨论的业务输入假设：

- 启用通道按通道序号排序，并沿逆时针方向均分扇区；
- 两通道示例中，通道 1 从九点钟方向、通道 2 从三点钟方向开始逆时针扫描；
- 当时约定奇数触发归为波长 1、偶数触发归为波长 2；
- DAS 输入预处理目标参考 ogprog 线性实时成像实例；
- 组包计数以单通道 A-line 数量为主要语义，而不是把多卡数据块数量直接当逻辑扫描计数；
- A-line 采样点数/采样率应由前面板参数换算，而不是固化常数。

这些是**历史需求输入**，不是当前 FPGA/LabVIEW 协议证明。尤其触发奇偶/扇区起点等内容，若与当前 source、现场配置或新硬件证据不一致，应以当前事实为准。

## 5. 与后续实现的关系

后续工程工作已引入并验证：

- Ring SHM/round observability；
- RoundIdentity hard barrier；
- source/reconstruction completion 分离；
- PhysicalRoundNormalizer；
- startup filter / variable-length round；
- timeout capture-before-reset / stale cutoff；
- 真实 ImagingSvc + CUDA selftest。

因此本归档的主要用途是解释“为什么后来强调 ownership、barrier、reset 和 observability”，而不是指导新的代码修改。
