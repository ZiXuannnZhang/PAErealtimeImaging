# RoundIdentity 阻塞点 3 收敛与 Final Marker 可靠性整改

日期：2026-09-16

## 0. 执行入口

仓库：`ZiXuannnZhang/PAErealtimeImaging`

实现分支：

```text
codex/physical-round-normalizer-integrated-20260916
```

本任务必须从以下精确 HEAD 继续：

```text
23debab2bf64473e5500231689113e567b3ca229
```

该分支与原 RoundIdentity 实现链已有正常共同祖先：

```text
merge-base = b4376203e7c9c8b369b6b87551f3718bd1edf424
```

当前 integrated 分支相对该 merge-base：ahead 2 / behind 0。

开始前必须执行并记录：

```bash
git fetch origin
git checkout codex/physical-round-normalizer-integrated-20260916
git status --short
git rev-parse HEAD
git merge-base HEAD b4376203e7c9c8b369b6b87551f3718bd1edf424
```

如果 `HEAD != 23debab2bf64473e5500231689113e567b3ca229`，不要自动 reset/rebase；停止并报告基线发生变化。

---

# 1. 当前复核结论

三个原生产级阻塞点中：

1. ImagingSvc reconstruction accumulator 跨物理轮污染：**已解决**。
2. snapshot metadata 与 SHM pixels 错配：**已解决**。
3. UI 用 delivered snapshot 数量 modulo 推断 final：**原始缺陷已解决，但 completion 语义尚未完全闭环**。

此外，当前 `roundComplete` 作为 one-shot classification signal 还存在一个次级可靠性风险：如果最后 logical trigger 首个被 Normalizer 分类的 card 不参与当前 Ring enabled-channel 路径，唯一的 `roundComplete=true` 可能在进入 Ring assembler 前被过滤，导致该轮实际完整但永远没有 final marker。

本任务只处理阻塞点 3 的剩余闭环和该次级风险。

---

# 2. 严格冻结项

以下内容已经通过复核，本任务不得重新设计或回退：

```text
RoundIdentity = (measurementSession, roundGeneration)
TriggerGroup physicalRoundIdentity
RingBlockAssembler active-round hard barrier
RingBlockAssembler stale old-round drop
RingBlockAssembler new identity residual discard / phase reset
ImagingSvc RingReconRoundState active identity barrier
ImagingSvc stale/closed identity rejection
TimeoutBoundary -> ring_reset -> svc close/reset
Controller input SHM block_seq/block_ready fail-closed
Controller output snapshot frame_seq/pixels same-lock copy
RingSnapshotCopy
submit_index timeout stale cutoff
RingRoundPresentation exact RoundIdentity lookup
CountBoundary 不直接 reset assembler / ImagingSvc
CUDA 数值算法
SHM ABI / binary save format
FPGA / UDP protocol
START admission fence
AutoSaveRoundCoordinator mapping core
```

尤其禁止为了 final 问题重新引入：

```text
snapshotsThisRound % blocksPerFrame
UI callback 数量推断物理轮完整性
CountBoundary 立即清空 Ring data plane
mutable latest-round 推断 ownership
```

---

# 3. P0：区分 source round end 与 reconstruction complete

## 3.1 当前错误语义

当前链路把同一个布尔量一路透传：

```text
PhysicalRoundNormalizer roundComplete
  -> TriggerGroup
  -> RingBlockAssembler
  -> ring_block_ready.round_complete
  -> ImagingSvc
  -> ring_snapshot_ready.round_complete
  -> MainWindow frameEnd / PNG / presentation
```

但 Normalizer 的 `roundComplete` 只证明：

```text
该 physical round 的最后 logical trigger 已被观察到。
```

它不能证明 ImagingSvc 实际消费了本轮全部 Ring blocks。

当前 ImagingSvc 在处理 final source block 后：

```text
++m_ringBlockIndex
写 snapshot
sendRingSnapshotToHost(..., roundComplete)
然后才检查 m_ringBlockIndex != m_ringBlocksPerFrame
```

所以如果中间一个 Ring block 因 SHM latest-wins / notification gap 丢失，但最后 source-final block 到达：

```text
expected = 5
实际 svc 收到 B1 B2 B4 B5(final)
m_ringBlockIndex = 4
sourceRoundComplete = true
```

当前仍会先向 UI 发布：

```text
round_complete = true
```

导致不完整 reconstruction 被允许执行 final PNG / presentation transition。

这就是本任务 P0。

## 3.2 必须建立的语义

在 ImagingSvc 中明确区分：

```text
sourceRoundComplete
= 上游数据面告诉服务：该物理轮已经到达 source end

reconstructionComplete
= sourceRoundComplete
  && 本轮服务端实际成功消费的完整 Ring block 数满足 configured expected blocks
```

对外 `ring_snapshot_ready.round_complete` 的唯一业务含义必须改为：

```text
reconstructionComplete
```

而不能继续表示 sourceRoundComplete。

## 3.3 必须满足的生产规则

### 正常完整轮

如果：

```text
sourceRoundComplete == true
m_ringBlockIndex == m_ringBlocksPerFrame
```

则：

```text
reconstructionComplete = true
snapshot.round_complete = true
允许 MainWindow final PNG / exact presentation completion
随后 close active RoundIdentity
随后 resetRingRecon()
```

### 源轮已结束但 reconstruction 不完整

如果：

```text
sourceRoundComplete == true
m_ringBlockIndex != m_ringBlocksPerFrame
```

则必须：

```text
reconstructionComplete = false
snapshot.round_complete = false
不得触发 final PNG
不得 complete presentation transition
必须记录明确 mismatch diagnostic
必须 close 当前 RoundIdentity
必须 resetRingRecon()
```

即：

```text
source end 决定 lifecycle closure
reconstruction complete 决定 final-image business action
```

两者不得混用。

### 重要约束

即使 reconstruction 不完整，也绝对不能等待下一轮数据“补齐”。

sourceRoundComplete 到达后仍必须结束旧 accumulator；下一轮必须从 clean reconstruction state 开始。

## 3.4 推荐最小实现

优先保持当前 IPC 结构不扩张。

可在 `ImagingSvc::processRingPulse()` 内采用等价逻辑：

```cpp
const bool sourceRoundComplete = roundComplete;
...
++m_ringBlockIndex;
const bool reconstructionComplete =
    sourceRoundComplete &&
    m_ringBlockIndex == m_ringBlocksPerFrame;

sendRingSnapshotToHost(..., reconstructionComplete);

if (sourceRoundComplete) {
    if (!reconstructionComplete) {
        // round_block_count_mismatch diagnostic
    }
    m_ringRound.close();
    resetRingRecon();
}
```

变量名可调整，但语义必须清晰。

不要新增第二套 UI round counter。

如为诊断需要增加 `source_round_complete` 字段，可以增加；但 UI 的 final gate 只能消费 `round_complete == reconstructionComplete`。

---

# 4. P1：修复 one-shot final marker 被未启用 card 吞掉的风险

## 4.1 当前风险

Normalizer 当前对第一次出现的最后 logical trigger 返回：

```text
roundComplete = true
```

而对同一 `(measurementSession, triggerSeq)` 的 cached 后续分类明确返回：

```text
roundComplete = false
```

这是为了保证 CountBoundary/控制事件 one-shot，本身合理。

但 Ring 数据面可能只消费部分 card/channel。

例如最后 logical trigger：

```text
Card 3 首先到达
Normalizer 首次分类 -> roundComplete=true
但 Card 3 当前 Ring channel 全部 disabled
ImagingBypass 过滤该 frame

之后 Card 0 到达
Normalizer cached classification -> roundComplete=false
Card 0 是 enabled Ring card
```

则本轮完整 Ring 数据可能全部形成，但 final marker 已经随 disabled card 丢失。

后果：

```text
无跨轮污染
但 final PNG / presentation 永远不完成
pending transition 只能等待过期/淘汰
```

## 4.2 不允许的修法

不要简单把现有 one-shot `roundComplete` 改成 cached 永远 true，然后让所有控制路径重复产生 boundary side effect。

控制事件 one-shot 与数据属性稳定性必须拆开。

## 4.3 推荐设计：新增稳定的“最后 logical trigger”数据属性

Normalizer classification 增加一个稳定字段，名称可自行选择，例如：

```text
isFinalLogicalTrigger
roundFinalLogicalTrigger
isRoundTerminalLogicalTrigger
```

语义：

```text
该 trigger identity 是否是所属 physical round 的最后一个 logical trigger。
```

要求：

- 第一次 classification 为最后 logical trigger 时，该字段为 true；
- cached 后续 card / late duplicate 对同一 trigger identity 仍返回 true；
- 它是稳定 data-plane metadata，不是 one-shot observer event；
- 原有 `roundComplete` one-shot 语义可以保留给控制/事件兼容路径；
- `CountBoundary` observer 仍然只触发一次。

推荐模型：

```text
roundComplete
= one-shot classification/control pulse（兼容旧控制语义）

isFinalLogicalTrigger
= stable per-trigger data property（Ring data plane 使用）
```

然后把 Ring final-marker 数据链切换为稳定字段：

```text
PhysicalRoundClassification.isFinalLogicalTrigger
  -> FrameConverter
  -> TriggerGroup.isFinalLogicalTrigger
  -> MainWindow Ring feed
  -> RingBlockAssembler PendingTrigger OR
  -> BlockCallback sourceRoundComplete
  -> ImagingController
  -> ImagingSvc
```

如果为了减少接口改名，可以让 TriggerGroup/Ring 层继续叫 `roundComplete`，但来源必须是上述稳定 data property，而不能继续直接来自 one-shot pulse。优先推荐显式区分名称，避免再次产生语义混淆。

## 4.4 Assembler 行为保持

当前 assembler 对 final trigger 多通道 fan-in 的：

```cpp
pt.roundComplete = pt.roundComplete || roundComplete;
```

以及 partial-final residual discard / require newer identity 行为应保留。

稳定 final marker 只负责保证 enabled card 无论第几个到达都能携带相同 terminal 属性，不得改变 assembler RoundIdentity barrier。

---

# 5. CountBoundary / TimeoutBoundary 职责不得变化

## CountBoundary

继续保持：

```text
产生一次控制事件 / save binding / presentation transition registration
不直接 reset RingBlockAssembler
不直接 reset ImagingSvc reconstruction
数据面由最后 trigger + 下一 RoundIdentity 驱动闭环
```

## TimeoutBoundary

继续保持：

```text
强制终止旧轮
assembler residual reset
ring_reset
ImagingSvc close/reset
submit_index stale cutoff
```

本任务不得把 CountBoundary 改成 TimeoutBoundary 式即时 reset。

---

# 6. Mandatory regression tests

至少新增/修改以下确定性测试。测试名称可调整，但覆盖语义不得减少。

## C1 — complete reconstruction final

配置 expected blocks = B。

服务语义模拟/实际 seam：

```text
收到 B 个同 RoundIdentity blocks
第 B 个 sourceRoundComplete=true
```

断言：

```text
reconstructionComplete=true
仅第 B 个 snapshot round_complete=true
随后 active round closed
reconstruction reset
```

## C2 — missing middle block + source final still arrives（本任务核心）

```text
expected B=5
服务实际消费 B1 B2 B4 B5(final)
```

断言：

```text
最终 snapshot round_complete=false
产生 round_block_count_mismatch
不得产生 final-business signal
旧 round 仍 close/reset
下一 RoundIdentity 第一 block 从 clean accumulator 开始
```

这一条必须直接覆盖当前剩余 blocker，不能只测试 UI helper。

## C3 — exact expected count only

验证：

```text
sourceRoundComplete=true && count < expected -> false
sourceRoundComplete=true && count == expected -> true
```

如果 count > expected 在合法路径理论上不应发生，也必须 fail closed：

```text
round_complete=false + diagnostic
```

不得用 `>=`。

## C4 — final snapshot lost

保留现有回归：

```text
旧轮 final snapshot 被 display latest-wins 丢失
下一轮普通 snapshot 不得补成旧轮 final
```

## M1 — stable terminal marker across cached cards

Normalizer：

```text
最后 logical trigger 第一次 classify
-> one-shot roundComplete=true
-> stable terminal property=true

同一 trigger cached classify
-> one-shot roundComplete=false
-> stable terminal property仍=true
```

同时断言 CountBoundary observer 只产生一次。

## M2 — disabled card first / enabled card later

模拟：

```text
最后 logical trigger first classification 落在 Ring-disabled card
该 frame 不送 assembler
同 trigger 的 enabled card 后到
```

断言 enabled card 仍携带 stable terminal property，最终 assembler 形成的完整 block 可以正确标记 sourceRoundComplete=true。

这条必须证明 one-shot signal 不再决定 Ring final 是否存在。

## M3 — duplicate/late terminal cards idempotent

同 final trigger 多个 card 都携带 stable terminal=true 时：

```text
PendingTrigger OR 正常
只形成一个 final block callback
final 后迟到同 identity 数据不能 reopen closed/residual round
```

## T1 — timeout regression

TimeoutBoundary 后：

```text
旧 identity 继续 stale-drop
新 identity clean start
submit_index stale cutoff 保持
```

## R — existing regressions

至少继续通过：

```text
ring_production_blockers_test
ring_block_assembler_test
ring_round_identity_test
ring_round_ui_state_test
ring_shm_observability_test
physical round normalizer / frame converter 相关 tests
```

如果仓库完整 CTest 可执行，执行完整 CTest，不只跑新增测试。

---

# 7. ImagingSvc/CUDA 验证

保留现有真实 ImagingSvc + CUDA identity-jump 自检，证明 blocker 1 没有回归。

此外，本任务至少需要一个服务级验证能够证明：

```text
中间 block 缺失
最终 source-final block 仍到达
=> ring_snapshot_ready.round_complete=false
=> 下一轮 clean start
```

实现方式可以是：

- 扩展 `ring_svc_selftest` 增加 deterministic drop/gap 参数；或
- 增加可独立测试的 service completion seam，并同时保留生产路径调用该 seam。

禁止只在测试代码里复制一份判断逻辑而生产代码不用它。

硬件采集卡/FPGA/NIC 实机验证仍标记：

```text
HARDWARE_VALIDATION_PENDING
```

---

# 8. 代码范围建议

预计允许修改范围：

```text
PhysicalRoundNormalizer.h/.cpp
DataTypes.h
FrameConverter.cpp（及必要 header）
RingBlockAssembler.h/.cpp（仅 final marker 接口语义所需，禁止改 barrier core）
MainWindow.cpp（仅 feed 字段切换）
ImagingController.h/.cpp（如命名/字段透传需要）
ImagingSvc.h/.cpp
RingRoundIdentity.h（如仅增加诊断字段需要）
相关 tests / selftest / CMake tests
```

如果需要修改范围明显超出这些文件，报告原因，不要顺手重构。

---

# 9. 诊断要求

对 source end 但 reconstruction 不完整的情况，必须有机器可读诊断，至少包含：

```text
measurement_session
round_generation
blocks_consumed
expected_blocks
source_round_complete=true
reconstruction_complete=false
```

可继续使用现有：

```text
round_block_count_mismatch
```

但信息必须足够区分“物理轮结束”和“最终图不完整”。

stable terminal marker 不要求每个 card 都输出日志，避免热路径日志放大。

---

# 10. 完成标准

本任务只有同时满足以下条件才可标记完成：

```text
1. ImagingSvc 不再把 sourceRoundComplete 直接作为 snapshot.round_complete。
2. snapshot.round_complete 仅在 source end + exact expected block count 时为 true。
3. source end + block mismatch 时仍 close/reset old reconstruction，但不触发 final PNG/presentation。
4. final logical trigger 的 data-plane terminal 属性对 cached cards 稳定。
5. disabled card first / enabled card later 不再丢 Ring final marker。
6. CountBoundary observer 仍 one-shot，且不直接 reset data plane。
7. TimeoutBoundary / submit_index stale cutoff 无回归。
8. blocker 1 reconstruction identity barrier 无回归。
9. blocker 2 SHM seq/pixels binding 无回归。
10. mandatory tests 通过。
11. Windows build 通过。
12. 硬件状态保持 HARDWARE_VALIDATION_PENDING，除非真实完成采集卡验收。
```

---

# 11. 提交与推送纪律

继续在：

```text
codex/physical-round-normalizer-integrated-20260916
```

实现，不另起孤立仓库/孤立历史。

禁止：

```text
force push
rebase 已发布 integrated 历史
reset 到独立副本 commit
把 ec1545c3... 独立历史重新 merge 进来
```

推荐：

1. 一个源码+测试 commit；
2. 一个报告/回执 commit（如果需要）；
3. 普通 fast-forward push。

源码 commit message 建议：

```text
Close Ring final reconstruction completion semantics
```

---

# 12. 最终报告

新增：

```text
CODEX_REPORTS/round-identity-final-completion-closure-20260916.md
```

报告必须明确列出：

```text
BASE_SHA
SOURCE_SHA
REPORT_SHA（若有）
REMOTE_BRANCH
REMOTE_HEAD_SHA
```

并分别回答：

```text
A. sourceRoundComplete 的定义是什么？
B. reconstructionComplete 的定义是什么？
C. 哪一个字段现在控制 ring_snapshot_ready.round_complete？
D. 中间 block 丢失但 final source block 到达时实际行为是什么？
E. final logical trigger 首个到达的是 disabled card 时，enabled card 如何仍获得 terminal metadata？
F. CountBoundary 为什么仍不会直接 reset assembler/service？
G. TimeoutBoundary 行为是否保持？
H. blocker 1 / blocker 2 的回归测试结果？
I. 硬件验收状态？
```

最终状态必须使用以下之一，不得模糊表述：

```text
CODE_IMPLEMENTED
AUTOMATED_TESTS_PASS / AUTOMATED_TESTS_FAIL
WINDOWS_BUILD_PASS / WINDOWS_BUILD_FAIL
HARDWARE_VALIDATION_PENDING / HARDWARE_VALIDATION_PASS
```

完成推送后返回精确远端 HEAD SHA，供独立复核。