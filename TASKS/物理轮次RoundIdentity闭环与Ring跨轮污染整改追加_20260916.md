# TASK ADDENDUM — 物理轮次 RoundIdentity 闭环与 Ring 跨轮污染整改

## 0. 任务身份

本文件是“物理轮次归一化 / CountBoundary 自动保存绑定 / Ring 超时边界”任务链的继续追加，处理两个必须作为**同一套 round-identity 闭环**解决的问题：

1. `partial trigger → CountBoundary → Ring 残块跨轮污染`；
2. reconstruction snapshot 缺少物理轮次身份，导致 presentation transition 依赖“当前最新 boundary”推断归属。

关联任务：

- `TASKS/物理轮次归一化与首控制触发安全过滤_20260914-043019.md`
- `TASKS/物理轮次归一化实施授权追加_20260914-110200.md`
- `TASKS/物理轮次超时重置语义整改追加_20260914.md`
- `TASKS/物理轮次业务边界统一整改追加_20260915-130400.md`
- `TASKS/物理轮次并发边界收口_20260915.md`
- `TASKS/CountBoundary自动保存轮次绑定整改追加_20260915-200300.md`

本任务不要求执行代理判断审核结论。执行代理只负责：

```text
按指定 continuation baseline 实施
运行指定测试 / 构建
记录客观结果
推送源码与执行报告
```

不得把本任务拆成两个互不关联的局部补丁。Ring 数据面隔离与 snapshot/presentation 归属必须使用同一物理轮次身份模型。

---

# 1. 分支与 continuation baseline

**CONTINUE_BRANCH**

```text
codex/physical-round-normalizer-20260914-043019
```

**CONTINUE_FROM_SHA**

```text
6f5f56372e5180f29fe2ce679c494075161fe88f
```

开始前必须执行：

```powershell
git fetch --prune origin
git rev-parse origin/codex/physical-round-normalizer-20260914-043019
git show origin/codex/task-docs:TASKS/物理轮次RoundIdentity闭环与Ring跨轮污染整改追加_20260916.md
```

如果实现分支远端 HEAD 不等于：

```text
6f5f56372e5180f29fe2ce679c494075161fe88f
```

立即停止并报告：

```text
CONTINUATION_BASELINE_BLOCKED
```

不得自行 reset / rebase / force / cherry-pick 猜测续接点。

实施与测试均直接继续在：

```text
codex/physical-round-normalizer-20260914-043019
```

上完成。

---

# 2. 当前生产事实与问题模型

## 2.1 Normalizer 的计数域包含 partial CardFrame

当前 `HostOutput::card()` 对进入 host output 的 `CardFrame` 调用：

```text
PhysicalRoundNormalizer::classify(
    measurementSession,
    triggerSeq,
    normalizationTime)
```

该调用不以 `Frame::complete` 为前置条件。

因此一个由 SourceCore packet assembly timeout / trigger switch 关闭出来的 incomplete frame，只要对应的是新的 physical trigger identity，也会进入 normalizer 并推进：

```text
physicalDistinctObserved
currentLogicalDistinctCount
logicalDistinctAccepted
```

若它恰好使当前 logical count 达到配置值 N，则 normalizer 仍可产生：

```text
roundComplete = true
CountBoundary(event.roundGeneration = G + 1)
```

本任务不得通过“让 partial 不再进入 normalizer”规避问题；该物理轮次分类语义保持不变。

## 2.2 SourceCore 的 Ring/sync 数据域只包含 multi-card complete identity

当前 SourceCore 对 incomplete frame 的顺序是：

```text
close card assembly
→ deliverCard(frame)
→ HostOutput::card(frame)
→ normalizer classification

if (!frame.complete)
    return
```

只有 complete frame 才进入跨卡 pending sync；只有同一 trigger identity 的所有卡均完整时，才产生：

```text
HostOutput::sync(...)
→ sync worker
→ DataProcessor::deliverAssembled(..., displayAndRing=true)
→ ImagingBypass
→ RingBlockAssembler
```

因此生产系统存在两个不同的数据域：

```text
PhysicalRoundNormalizer logical count domain
    = 所有可见 distinct physical identities（包含 partial card identity）

Ring multi-card data domain
    = 只有形成完整 multi-card sync 的 identities
```

现有测试已经覆盖“assembly timeout partial 会进入 normalizer、但不会进入 sync/Ring”的隔离语义；本任务必须继续覆盖其跨 CountBoundary 后果。

## 2.3 当前 Ring assembler 不携带 physical round identity

当前 `RingBlockAssembler` 输入核心为：

```text
pushChannelLine(channelId, triggerSeq, line, length)
```

Block callback 核心为：

```text
(raw, angles, channels, blockSeq)
```

两者都没有：

```text
measurementSession
roundGeneration
```

当前 `completeLogicalRound()` 仅在：

```text
m_pending.empty()
&& m_blockTriggers == 0
```

时才能完成 phase reset。

如果旧轮应有 N 个 logical identities，而 Ring 因 partial / sync eviction / downstream drop 少收到一个，则可形成：

```text
old round residual:
    m_blockTriggers > 0
    或 m_pending 非空

Normalizer:
    已产生 CountBoundary(G+1)
```

下一轮数据继续进入 assembler 时，当前实现缺少按 physical round identity 的硬隔离条件，存在：

```text
OLD round residual + NEW round trigger
→ 同一个 Ring block
```

的结构性风险。

本任务必须保证：

> 一个 Ring block 在任何情况下都只能属于一个 `(measurementSession, roundGeneration)`。

不得通过补零、伪造 missing trigger、复制上一触发或按 `triggerSeq` 数值顺序猜测轮次来满足这一条件。

## 2.4 CountBoundary observer 不能作为立即清 Ring 的时序点

CountBoundary 可能在同一 final physical identity 的第一张卡完成 `HostOutput::card()` 分类时已经产生；同一 identity 其它卡，以及随后形成的 multi-card sync，可能更晚到达 Ring path。

所以以下方案禁止：

```text
CountBoundary observer 到达
→ 立即 reset RingBlockAssembler
→ 清空旧轮残块
```

因为该做法会把仍在途中的合法旧轮 final sync 一并清掉。

CountBoundary event 是**控制面轮次转换声明**，但 Ring 数据面的 hard barrier 必须由带身份的数据自身或等价的严格有序 barrier 驱动。

## 2.5 当前 snapshot 协议只有 submit_index，没有 physical round identity

当前 Ring producer/controller → ImagingSvc 的 `ring_block_ready` 主要携带：

```text
seq
submit_index
submit_wall_us
```

当前 ImagingSvc → controller 的 `ring_snapshot_ready` 主要携带：

```text
seq
submit_index
```

当前 `ringSnapshotReady(...)` signal 也没有：

```text
measurementSession
roundGeneration
```

因此 reconstruction snapshot 的语义归属只能由主进程结合“当前/latest boundary、当前 UI state、submit index cutoff”等外部状态推断。

`submit_index` 必须继续保留用于 FIFO / stale cutoff；但它不是 physical round identity，不能替代：

```text
(measurementSession, roundGeneration)
```

## 2.6 CountBoundary presentation transition 不能用 latest/global 状态猜测

CountBoundary 自动保存整改后，数据路由已经按：

```text
(measurementSession, roundGeneration)
→ sessionGen / directory
```

绑定。

presentation/reconstruction 侧也必须采用同一 round identity 选择对应 transition。

当多个 CountBoundary 已发生、旧 reconstruction snapshot 延迟返回时，禁止使用：

```text
latest boundary
latest round generation
current directory
current auto-save generation
单一 m_ringBoundaryPending 槽
```

推断旧 snapshot 属于哪一轮。

---

# 3. 权威身份模型

本任务统一定义逻辑概念：

```text
RoundIdentity := (
    measurementSession,
    roundGeneration
)
```

实际 C++ 类型名称与文件位置由执行代理决定，可以是：

```text
PhysicalRoundIdentity
RoundIdentity
RingRoundIdentity
```

或等价结构。

但必须满足以下不变量。

## 3.1 LogicalScan identity

每个 normalizer 分类后的 `LogicalScan TriggerGroup` 已拥有：

```text
measurementSession = S
roundGeneration    = G
logicalTriggerIndex
```

Ring path 必须使用该数据自身携带的 `(S,G)`，不能重新根据 triggerSeq、最新 boundary 或 UI 状态计算轮次。

## 3.2 CountBoundary identity 语义

当前 normalizer CountBoundary 发生时：

```text
旧 logical data round = (S,G)
CountBoundary event.roundGeneration = G+1
```

因此 presentation transition / boundary record 必须能无歧义表达：

```text
oldRound  = (S,G)
nextRound = (S,G+1)
```

可以显式存两端，也可以在经过检查的前提下从 `event.roundGeneration` 推导 old generation；不得把 event 的 `G+1` 与旧 snapshot 的 `G` 混用。

## 3.3 Session 是身份的一部分

以下两者必须视为完全不同的身份：

```text
(session 100, generation 0)
(session 101, generation 0)
```

measurement restart / source restart 后不得仅按 roundGeneration 匹配旧 Ring/snapshot/presentation 状态。

## 3.4 JSON / IPC 必须无损传输

如果 RoundIdentity 通过 JSON/ZMQ 传输：

```text
measurementSession
roundGeneration
```

不得经过会造成 64-bit integer 精度丢失的 double-only 序列化。

允许：

```text
decimal string
已证明安全的 exact integer encoding
```

或其它无损方案。

实施报告必须说明实际 wire 字段与解析方式。

---

# 4. Ring 数据面 hard barrier

## 4.1 每个输入 group 都必须带 RoundIdentity 进入 Ring assembler 边界

当前 MainWindow / ImagingBypass 已消费 `TriggerGroupConstPtr`，该 group 包含 normalizer metadata。

需要把：

```text
frame.measurementSession
frame.roundGeneration
```

传入 Ring 数据面身份控制。

可以修改：

```text
RingBlockAssembler::pushChannelLine(...)
```

使其携带 identity，也可以在 assembler 外建立 round-aware feeder/barrier；实际 API 名称不限。

关键要求：

> assembler 接收一根 channel line 之前，必须已经知道它属于哪个 RoundIdentity。

## 4.2 active Ring round

Ring 数据面维护明确的 active identity，例如：

```text
activeRound = none
```

首个合法 LogicalScan 输入 `(S,G)`：

```text
activeRound := (S,G)
```

后续同 identity 正常组包。

若输入 identity 与 activeRound 相同：

```text
正常处理
```

若输入 identity 是严格后继的新轮，例如：

```text
activeRound = (S,G)
incoming    = (S,G+1)
```

在向 assembler 写入 incoming 的任何 channel line **之前**，必须执行一次 round transition barrier。

## 4.3 clean transition

如果旧轮状态已经干净：

```text
m_pending.empty()
m_blockTriggers == 0
```

则按现有正常 CountBoundary 语义完成 phase reset：

```text
old round phase finalized/reset
activeRound := incoming identity
feed incoming
```

不得丢弃旧轮已经完整形成的 block。

## 4.4 residual transition

如果 incoming 已经属于新轮，但旧 active round 仍有：

```text
m_pending 非空
或
m_blockTriggers != 0
```

则说明旧轮 Ring 数据域不完整。

必须：

```text
1. 记录一次低频结构性诊断
2. 丢弃 / 清空 OLD round residual assembler state
3. 重置 wavelength / angle / per-round phase
4. activeRound := incoming identity
5. 再 feed NEW round 数据
```

禁止：

```text
OLD residual + NEW data 拼块
```

禁止为了凑满旧 block：

```text
补 0
复制 trigger
伪造 channel line
把 NEW data 当 OLD data
```

这属于缺失数据的 fail-closed 隔离，不是数据修复。

## 4.5 stale old-round input

一旦 activeRound 已推进到 `(S,G+1)`，之后若收到：

```text
(S,G)
```

的迟到 Ring group/channel line，必须：

```text
reject/drop
记录低频 stale-round 诊断/计数
不得 reset 回旧轮
不得进入 block
```

跨 measurementSession 的旧数据同理。

## 4.6 非连续 generation

如果出现：

```text
active = (S,G)
incoming = (S,G+k), k>1
```

不得尝试虚构中间轮次。

允许直接执行 residual/clean barrier 后推进到 incoming identity，但必须记录 generation gap 诊断。

如果执行代理认为产品不允许该情况，也可以 fail closed；无论采用哪一种，测试和报告必须明确行为。

## 4.7 CountBoundary observer 的角色

CountBoundary observer 可以继续用于：

```text
auto-save binding
presentation transition registration
诊断
UI frame-end expectation
```

但它不再是 Ring 数据面“现在立即清 assembler”的权威时序点。

Ring hard barrier 必须依赖实际 incoming data identity 或等价的严格有序 data-plane barrier。

---

# 5. Ring block 必须携带 RoundIdentity

## 5.1 Block callback metadata

每个 assembler 完整输出 block 必须有确定且唯一的：

```text
RoundIdentity (S,G)
```

Block callback / block descriptor 必须携带该身份。

一个 block 中如果检测到来自不同 identity 的 line，必须视为内部 invariant violation 并 fail closed；不得提交给 ImagingSvc。

## 5.2 Block sequence 与 identity 解耦

保持现有：

```text
blockSeq / submit_index
```

观测语义；不要求因为 round transition 重置全局 submit_index。

但：

```text
blockSeq / submit_index != RoundIdentity
```

任何下游归属判断不得用 blockSeq 或 submit_index 反推出 roundGeneration。

## 5.3 phase reset

每次 physical round identity 从 `(S,G)` 正式推进到新 identity 时，下一轮第一根有效 trigger 的 wavelength/angle phase 必须从该轮定义的起点重新开始。

旧轮因 partial 留下 residual 时，也必须先清 residual 再 reset phase，不能让旧 `m_globalTrigger` 泄漏到下一轮。

---

# 6. RoundIdentity 传到 ImagingSvc 并原样返回

## 6.1 Producer → ImagingSvc

扩展 `submitRingBlock(...)` 或等价 block descriptor，使每个 `ring_block_ready` 除现有字段外携带：

```text
measurement_session
round_generation
```

字段名可调整，但必须稳定、可诊断、无损。

期望语义：

```text
ring_block_ready.submit_index = X
ring_block_ready.RoundIdentity = (S,G)
```

这个 identity 必须来自提交 block 本身，不能在 `ImagingController` 内读取“当前 MainWindow round”。

## 6.2 ImagingSvc

ImagingSvc 收到 `ring_block_ready` 后：

```text
parse identity
validate identity
associate it with this submit_index / processed block
```

snapshot 是由哪个 submit/block 更新产生，就必须把那个 block 的 RoundIdentity 原样带回。

禁止 ImagingSvc 使用：

```text
自己的 current round counter
seq 推算
收到 reset 次数推算
```

替代 producer 提供的 identity。

## 6.3 ImagingSvc → Producer snapshot

`ring_snapshot_ready` 必须至少携带：

```text
seq
submit_index
measurement_session
round_generation
```

Controller 解析后，`ringSnapshotReady` signal 或等价 metadata object 必须把 RoundIdentity 传到 MainWindow。

## 6.4 identity 缺失/非法

生产 Ring 模式下，如果 snapshot message 缺少 RoundIdentity、字段非法、无法无损解析，禁止：

```text
fallback 到 latest/current RoundIdentity
fallback 到 current presentation directory
```

应：

```text
fail closed for boundary/presentation application
记录低频诊断
释放/处理 snapshot buffer 生命周期，避免卡住 worker
```

是否仍允许仅显示图像由执行代理决定，但不得让无身份 snapshot 触发 auto-save presentation transition 或旧轮 PNG 归属变更。

---

# 7. Snapshot → presentation exact match

## 7.1 presentation transition 必须按 old RoundIdentity 索引

CountBoundary 注册的 presentation transition 至少包含：

```text
oldRound  = (S,G)
nextRound = (S,G+1)
oldSessionGen / oldDirectory
newSessionGen / newDirectory
boundary kind
```

多个尚未完成的 CountBoundary transition 必须能够同时存在。

可以采用：

```text
map<RoundIdentity, Transition>
deque + exact identity lookup
```

或等价 bounded structure。

不得只保存一个“latest pending transition”作为正确性依据。

## 7.2 exact snapshot ownership

当 snapshot `(S,G)` 返回并满足现有 final-frame/frame-end 条件时：

```text
lookup transition by exact oldRound=(S,G)
```

只允许消费该条 transition。

不得因为当前已到 `(S,G+2)`，就让 `(S,G)` snapshot 消费 `(S,G+1)->(S,G+2)` transition。

## 7.3 PNG directory

旧轮 final snapshot `(S,G)` 的 PNG 必须使用该 transition 保存的：

```text
oldDirectory
```

不能读取 mutable `m_reconSaveDir` 作为 round ownership 的唯一依据。

## 7.4 presentation apply

旧 round final snapshot 完成后，presentation target 可推进到 transition 的：

```text
newDirectory
```

但必须带 monotonic / stale protection。

例如如果：

```text
G+1 snapshot 已先完成并把 presentation 推进到 G+2
随后 G snapshot 才迟到
```

迟到 G snapshot：

```text
可以按策略保存属于 G 的 old PNG（若尚未保存且仍允许）
但不得把 current presentation target 从 G+2 倒退到 G+1
```

具体 presentation state 结构由执行代理决定，但必须有 deterministic test。

## 7.5 duplicate snapshot

同一 `(S,G)` final snapshot / callback 重复到达时：

```text
transition 不得重复消费
不得重复分配 auto-save generation
不得反复切目录
不得双重 frame reset
```

现有 idempotence 机制可复用或强化。

---

# 8. submit_index 与 RoundIdentity 的职责必须同时保留

本任务禁止把现有 `submit_index` stale cutoff 删除后只依赖 RoundIdentity。

两者职责不同：

```text
RoundIdentity
    = 语义归属：这块 / 这张 snapshot 属于哪个物理轮次

submit_index
    = producer FIFO / reset cutoff：这张 snapshot 是否在 reset 命令之前提交
```

TimeoutBoundary 下现有 stale cutoff / reset ordering 逻辑必须继续工作。

期望最终判断类似：

```text
先按 submit_index / reset epoch 判断是否是不可接受的 stale work
再按 RoundIdentity 决定 accepted snapshot 的业务归属
```

实际顺序可根据现有线程模型调整，但两个维度均不得丢失。

---

# 9. 生命周期与 bounded state

以下生命周期必须清理或重建 Ring round identity / presentation transition 状态：

```text
measurement session begin
measurement stop / restart
source listener rebuild
ImagingSvc stop / restart
Ring configuration restart
auto-save disable / reconfigure（仅 presentation binding 部分）
```

要求：

- 不得让旧 measurementSession 的 pending presentation transition 影响新 session；
- 不得让旧 active Ring identity 跨 ImagingSvc lifecycle 复用；
- pending transition / identity map 不得无限增长；
- 正常消费后及时删除；
- session/lifecycle reset 时整体清理对应状态。

---

# 10. 明确禁止的捷径

以下方案不得作为本任务实现：

## 10.1 不得修改 physical-round classification 来隐藏 partial

禁止仅为了 Ring 对齐而把：

```text
partial CardFrame
```

排除出 normalizer distinct/logical count。

本任务不修改物理轮次协议定义。

## 10.2 不得修改 SourceCore 让 partial 强行形成 sync

禁止：

```text
partial card + 补零其它卡
→ fake multi-card sync
```

或其它数据伪造。

## 10.3 不得 CountBoundary observer 立即无条件 reset assembler

该时序会误删仍在途的合法 old final sync。

## 10.4 不得按 triggerSeq 猜 RoundIdentity

`triggerSeq` 是 16-bit wire identity，允许 wrap；不得作为 generation counter。

## 10.5 不得用 latest/current global state 给 snapshot 补身份

禁止：

```text
snapshot missing identity
→ attach current/latest generation
```

## 10.6 不得只修 MainWindow presentation，不修 Ring block identity

即使 UI transition exact lookup 修好，如果 block 仍能跨 round 混合，本任务仍未完成。

## 10.7 不得只修 Ring residual，不把 identity 送到 snapshot

即使 assembler 不跨轮，如果 snapshot 仍无 RoundIdentity，presentation blocker 仍未完成。

---

# 11. 优先复用与冻结范围

## 11.1 应优先复用

现有已经完成的：

```text
TriggerGroup.measurementSession
TriggerGroup.roundGeneration
AutoSaveRoundCoordinator round binding
FileSaver roundGeneration rollover
submit_index / stale cutoff
PhysicalRoundNormalizer CountBoundary / TimeoutBoundary
```

应作为现有基础，不要另建第二套业务 generation。

## 11.2 原则上冻结

除非为编译接口适配、测试 seam 或诊断字段所必需，原则上不修改以下语义：

```text
PhysicalRoundNormalizer classification policy
SourceCore complete/partial assembly policy
SocketReceiver UDP/start fence 行为
FileSaver round-aware 数据路由语义
AutoSaveRoundCoordinator round binding 分配语义
```

特别是：

```text
CountBoundary save binding
```

不得退回“current generation”读取。

## 11.3 允许修改的主要文件范围

预计包括但不限于：

```text
MC_410T_MultiCard/delivery/include/DataTypes.h
或新增 neutral RoundIdentity header

MC_410T_MultiCard/delivery/include/RingBlockAssembler.h
MC_410T_MultiCard/delivery/src/RingBlockAssembler.cpp

MC_410T_MultiCard/delivery/include/ImagingController.h
MC_410T_MultiCard/delivery/src/ImagingController.cpp

MC_410T_MultiCard/delivery/src/ImagingSvc/ImagingSvc.h
MC_410T_MultiCard/delivery/src/ImagingSvc/ImagingSvc.cpp

MC_410T_MultiCard/delivery/include/MainWindow.h
MC_410T_MultiCard/delivery/src/MainWindow.cpp

必要时：
MC_410T_MultiCard/delivery/include/RingShmObservability.h
MC_410T_MultiCard/delivery/src/ImagingBypass.cpp
MC_410T_MultiCard/delivery/include/ImagingBypass.h
```

以及测试 / CMake 文件。

不强制共享内存 ABI 变更。RoundIdentity 优先通过已有 ZMQ control metadata 传输；如果执行代理认为必须修改 QSharedMemory header/layout，实施报告必须说明原因、兼容性影响与对应测试。

---

# 12. Required Automated Tests

以下场景必须进入自动化，不能只在执行报告中描述。

测试名称可调整，但语义必须一一覆盖。

## R1 — partial identity 计数后跨 CountBoundary，禁止残块拼入下一轮

使用小 N 和小 Ring block fixture 构造确定性场景，例如：

```text
N = 4 logical triggers / physical round
perChannelBlock = 4
```

OLD round `(S,G)`：

```text
control
logical 0 complete multi-card sync
logical 1 complete multi-card sync
logical 2 某卡 partial → normalizer 仍计数，但不形成 sync
logical 3 complete multi-card sync → normalizer 到 CountBoundary(G+1)
```

此时 Ring 旧轮只实际收到 3 个 complete sync trigger，形成 residual。

然后立即发送：

```text
next control
NEW round `(S,G+1)` 的 complete LogicalScan
```

检查：

```text
NEW round 第一个 trigger 不得把 OLD residual 补成 block
OLD residual 被一次性 discarded/reset
NEW round phase 从起点开始
后续 NEW round 形成的首个完整 block 只含 G+1 数据
block.RoundIdentity == (S,G+1)
```

使用可区分 old/new 的 raw 值，必须检查 block 实际内容，不只检查计数器。

## R2 — normal clean CountBoundary 不得过早清 final old sync

构造无丢失的正常轮：

```text
control + N complete logical sync
```

让 CountBoundary observer 在 final identity card classification 阶段正常先发生，而 final multi-card sync 随后到 Ring。

检查：

```text
final OLD sync 仍被完整接纳
OLD round 正常形成预期 block
无 residual discard
随后 NEW round 正常起始
```

此测试用于阻止“observer 一到就 reset assembler”的错误实现。

## R3 — stale old round after data-plane advance

构造：

```text
active 已推进到 (S,G+1)
随后注入一个迟到 (S,G) Ring group
```

检查：

```text
迟到 group 被 drop/reject
不进入 pending
不改变 blockTriggers
不把 activeRound 回退
记录 stale-round 诊断/计数
```

## R4 — block identity invariant

同一 round 生成多个 blocks，检查每个 block：

```text
有 RoundIdentity
所有 line 均属于该 identity
无 block 跨 generation
```

注入 identity mismatch 时必须 fail closed，不提交混合 block。

## R5 — Ring producer / ImagingSvc protocol identity echo

对至少两个不同 RoundIdentity：

```text
(S,G)
(S,G+1)
```

发送不同 `ring_block_ready`，检查：

```text
measurement_session / round_generation 无损到达 ImagingSvc
ring_snapshot_ready 原样返回相同 identity
submit_index 仍正确关联
```

测试必须覆盖字段不存在或非法：

```text
不得自动使用 current/latest identity
```

## R6 — 多个 CountBoundary outstanding + snapshot 延迟

注册至少两条 presentation transition：

```text
T0: (S,G)   → (S,G+1)
T1: (S,G+1) → (S,G+2)
```

在 T0 snapshot 尚未处理时先产生 T1。

随后按正常顺序返回 snapshot：

```text
snapshot (S,G)
snapshot (S,G+1)
```

检查每张 snapshot 只消费 exact transition。

## R7 — snapshot 反序返回不能 presentation rollback

沿用 R6，但让：

```text
snapshot (S,G+1) 先返回并推进 presentation 到 G+2
然后 snapshot (S,G) 迟到
```

检查：

```text
迟到 G snapshot 不得把 current presentation target 倒退到 G+1
不得消费 T1 两次
不得额外 allocate auto-save generation
```

如果旧 PNG 的产品语义允许迟到保存，必须验证其仍写 G 对应 oldDirectory；如果设计选择丢弃过时 PNG，也必须 deterministic 并记录诊断，不能写到 current/latest directory。

## R8 — duplicate final snapshot idempotence

同一 `(S,G)` final snapshot 重复投递：

```text
transition apply 一次
frame reset 一次
directory apply 一次
无额外 generation
```

## R9 — TimeoutBoundary regression

构造：

```text
partial OLD round
→ physical idle TimeoutBoundary
→ reset command / submit_index cutoff
→ NEW round data
→ OLD stale snapshot 延迟返回
```

检查：

```text
submit_index stale cutoff 仍生效
RoundIdentity 归属仍生效
NEW round block 不含 OLD residual
OLD snapshot 不影响 NEW presentation
```

## R10 — measurement session restart

构造：

```text
session A, generation 0
session B, generation 0
```

检查 Ring active identity、block metadata、snapshot transition 均不把两个 session 混为同一轮。

## R11 — sync/Ring drop 等价场景

除 SourceCore partial 外，再用 deterministic seam 模拟至少一种：

```text
sync eviction
ImagingBypass drop
或直接缺失一个 OLD complete sync trigger
```

随后进入新 RoundIdentity，检查 residual hard barrier 仍阻止跨轮拼块。

## R12 — 现有保存轮次绑定回归

以下既有语义必须继续通过：

```text
old round TriggerGroup → old sessionGen / directory
new round TriggerGroup → new sessionGen / directory
multi-card late final identity → 仍属于 old round
manual-save / auto-save off 不受影响
FileSaver physical round rollover 不退化
```

优先继续运行已有：

```text
count_boundary_save_binding_test
filesaver_round_boundary_test
paimage_host_output_test
auto_save_round_coordinator_test
```

## R13 — 既有 physical normalizer contract 回归

继续通过：

```text
physical_round_normalizer_test
```

本任务不得通过修改 normalizer 语义让新 Ring 测试通过。

## R14 — protocol / service selftest

如已有 RingSvc selftest / protocol test 能承载 identity wire contract，应扩展并运行。

至少验证：

```text
64-bit identity serialization
missing identity rejection
identity echo
submit_index coexistence
```

---

# 13. 诊断要求

只增加低频结构性诊断，不得为每个正常 trigger 写高频日志。

至少需要能够从诊断中回答：

```text
1. Ring active round 从哪个 identity 切到哪个 identity？
2. 切换时旧 round 是否 clean？
3. 若不 clean，丢弃了多少 pending trigger / blockTriggers？
4. 是否收到 stale old-round group？
5. 一个提交给 ImagingSvc 的 block 属于哪个 RoundIdentity？
6. snapshot 返回的 RoundIdentity 是什么？
7. snapshot 是否 exact-match 到 presentation transition？
8. transition 是 applied / duplicate / stale / missing 中哪一种？
```

建议字段：

```text
measurementSession
roundGeneration
oldMeasurementSession
oldRoundGeneration
newMeasurementSession
newRoundGeneration
blockSeq
submitIndex
pendingCount
blockTriggers
transitionAction
reason
```

名称可调整。

出现 residual discard 时必须有可统计 counter 或等价 snapshot 字段，便于后续实机日志确认是否发生过跨轮保护动作。

---

# 14. 性能与并发约束

## 14.1 Ring 热路径

RoundIdentity 检查是每个 Ring group/channel 的热路径逻辑，应保持 O(1) 或等价低开销。

不得：

```text
每 trigger 文件 I/O
每 trigger JSON 日志
无界 map 增长
UI 线程同步等待 Ring worker
```

## 14.2 锁顺序

不得新增：

```text
MainWindow UI mutex
Ring assembler mutex
ZMQ mutex
snapshot worker mutex
```

之间的循环锁依赖。

实施报告必须列出新增/变更的主要锁与调用方向。

## 14.3 receiver hot path

本任务不得把新的 Ring identity IPC 或 snapshot bookkeeping 放入 SocketReceiver packet hot loop。

normalizer observer 已有业务边界处理可保留，但不要增加新的每包阻塞工作。

---

# 15. 构建与测试要求

至少执行当前项目可用的 Windows / MinGW Debug 构建与 CTest。

建议顺序：

```powershell
cmake --build <existing-debug-build-dir> -j
ctest --test-dir <existing-debug-build-dir> --output-on-failure
```

并单独记录以下目标的结果：

```text
physical_round_normalizer_test
ring_block_assembler_test
ring_round_ui_state_test
paimage_host_output_test
count_boundary_save_binding_test
filesaver_round_boundary_test
auto_save_round_coordinator_test
相关新增 RoundIdentity / Ring protocol / snapshot tests
相关 RingSvc selftest（若纳入现有构建）
```

如果某测试因平台、CUDA、外部 DLL 或硬件环境无法执行：

```text
不得写成 PASS
```

必须在报告中记录：

```text
NOT_EXECUTED
原因
已执行的替代静态/单元验证
剩余需要的环境
```

本任务的软件自动化通过不等价于实机闭环完成；不得在报告中声称硬件实测已完成，除非确实执行并保留证据。

---

# 16. 实施报告

源码修改完成、测试执行完成后，在实现分支新增：

```text
CODEX_REPORTS/physical-round-normalizer-20260914/round-identity-closure-report.md
```

报告必须包含：

```text
CONTINUE_FROM_SHA
SOURCE_FIX_SHA
RECEIPT_SHA
```

以及：

1. changed files；
2. 最终 RoundIdentity C++ 表达；
3. Ring active-round barrier 实现位置；
4. clean transition / residual transition / stale drop 的实际行为；
5. block callback 如何携带 identity；
6. producer → ImagingSvc 的实际 ZMQ 字段；
7. ImagingSvc → producer snapshot 的实际 ZMQ 字段；
8. 64-bit identity 的无损序列化方法；
9. MainWindow presentation transition 的索引结构；
10. delayed / out-of-order snapshot 的处理规则；
11. submit_index stale cutoff 与 RoundIdentity 的职责分工；
12. session/service restart 状态清理点；
13. 新增诊断字段/counter；
14. 新增/变更锁与锁顺序；
15. 每一项 Required Test 的测试名与实际结果；
16. 完整 build/CTest 结果；
17. NOT_EXECUTED 项及原因；
18. 已知限制。

报告只记录客观实施与验证结果，不写最终审核结论。

---

# 17. 提交与回执规则

先提交全部源码与测试：

```text
SOURCE_FIX_SHA = <source + tests commit>
```

随后运行/确认测试并写报告，再创建只包含执行报告/回执必要修正的提交：

```text
RECEIPT_SHA = <report-only commit>
```

要求：

```text
RECEIPT_SHA 的 source tree 与 SOURCE_FIX_SHA 相同，
除 CODEX_REPORTS 下报告文件外不得再修改生产源码或测试源码。
```

如果测试后发现还需要改源码：

```text
不得把该改动塞进 receipt-only commit
```

应重新形成新的 SOURCE_FIX_SHA，重新执行受影响测试，再生成最终 RECEIPT_SHA。

最终推送：

```powershell
git push origin codex/physical-round-normalizer-20260914-043019
```

执行报告中记录远端最终 HEAD。

---

# 18. Stop / Block 条件

遇到以下情况停止扩展范围并报告，不自行重新定义产品语义：

## 18.1 Baseline 不一致

```text
CONTINUATION_BASELINE_BLOCKED
```

## 18.2 必须改变 normalizer physical identity contract 才能实现

例如需要把 partial 从 logical count 中移除、改变 first-visible control policy、改变 CountBoundary generation 语义。

报告：

```text
ROUND_CONTRACT_SCOPE_BLOCKED
```

并说明为什么下游 identity barrier 无法解决。

## 18.3 必须改变 FPGA / UDP wire protocol

本任务不修改采集卡 wire format。

报告：

```text
WIRE_PROTOCOL_SCOPE_BLOCKED
```

## 18.4 无法在不丢失 64-bit 精度的情况下传输 identity

不得静默降级为 double 或只传低位。

报告：

```text
ROUND_IDENTITY_SERIALIZATION_BLOCKED
```

## 18.5 发现共享内存 ABI 是唯一可行通道且兼容性无法证明

先报告：

```text
RING_IPC_ABI_BLOCKED
```

不要直接发布不兼容 ABI。

---

# 19. 最终验收不变量

源码与自动化测试必须共同证明以下不变量：

```text
I1.
一个 Ring block 只属于一个 RoundIdentity。

I2.
OLD round residual 永远不能由 NEW round trigger 补满。

I3.
CountBoundary observer 先发生时，合法的 OLD final sync 仍能进入 OLD round。

I4.
第一次看到 NEW RoundIdentity 时，如 OLD assembler 非 clean，
必须先丢弃 OLD residual / reset phase，再接纳 NEW data。

I5.
一旦 Ring activeRound 前进，stale OLD round data 不能让状态回退。

I6.
每个提交到 ImagingSvc 的 Ring block 都携带无损 RoundIdentity。

I7.
ImagingSvc 返回 snapshot 时原样携带产生该 snapshot 的 RoundIdentity。

I8.
MainWindow 只用 snapshot 自己的 RoundIdentity exact-match presentation transition。

I9.
多个 outstanding CountBoundary / snapshot 乱序返回时，
旧 snapshot 不能消费后继 transition，也不能把 presentation directory 倒退。

I10.
submit_index stale cutoff 继续工作；RoundIdentity 不替代其 FIFO/reset 职责。

I11.
现有 round-aware auto-save / FileSaver 数据路由语义不退化。

I12.
PhysicalRoundNormalizer / SourceCore partial 语义不因本任务被重新定义。
```

如果实现只能证明其中一部分，不得把任务报告为全部完成；报告中明确列出未满足不变量与阻塞原因。
