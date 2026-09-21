# 物理轮次 RoundIdentity 闭环三个生产级阻塞点根因调查 审核报告

- 审核日期：2026-09-16
- 审核对象：《物理轮次 RoundIdentity 闭环三个生产级阻塞点根因调查报告.md》
- 审核方式：对照源码逐条核实报告论断
- 源码位置：`D:\ChatGPT\PAERealtimeImaging\_worktrees\physical-round-normalizer-20260914-043019`
- 下文引用路径均相对于该 worktree 根目录

---

## 1. 审核结论（总体）

**原报告内容属实，结论与整改方向成立，可以采信。**

- 三个阻塞点的"基线原有、非本次任务引入"归因全部与源码证据一致；
- "不建议整体回退"的判断成立；
- "保留 identity 数据面、收缩 UI/presentation 计数状态机、将完整性判定下沉"的方向正确。

同时，审核发现原报告有 **1 处后果评估偏轻、1 处推荐方案不是最优、2 处实现细节需补充**，详见第 4、5 节。

---

## 2. 调查基线核实（对应原报告 §2）

| 原报告声明 | 核实结果 |
|---|---|
| 整改前基线为 `6f5f56372e5180f29fe2ce679c494075161fe88f` | 属实 |
| RoundIdentity 实现提交为 `76e773a824c1a82484b7214410cc63176d8494a2` | 属实（"Close Ring physical round identity across data and snapshots"，22 文件，+1406/-232） |
| 实现提交的父提交为指定 baseline | 属实，`76e773a` 的 parent 即 `6f5f563`，可直接区分 baseline 原有行为与新增行为 |
| 源码提交之后的提交不改变生产源码 | 属实，`33f2199`、`b437620` 仅改 `CODEX_REPORTS` 下文档 |

---

## 3. 逐项核实结果

### 3.1 阻塞点一：ImagingSvc reconstruction accumulator 跨轮污染（原报告 §4）——属实

现状代码证据：

- 仅在固定计数命中时重置：`ImagingSvc.cpp:748-749`
  `if (m_ringBlocksPerFrame > 0 && m_ringBlockIndex % m_ringBlocksPerFrame == 0) resetRingRecon();`
- `processRingPulse` 已接收 `RoundIdentity`（`ImagingSvc.cpp:557-559`），但只做 `valid()` 检查（`:562`）并透传给快照通知（`:741` 经 `sendRingSnapshotToHost(submitIndex, round)`），**无任何 activeRound 比对**，跨轮 barrier 确实未建。
- baseline 同位置为完全相同的取模重置（baseline `ImagingSvc.cpp:732-733`），"基线原有缺陷"成立。
- 边界分工与 §10 描述一致：CountBoundary 不重置 assembler/svc（`MainWindow.cpp:2325-2326` 注释明示 "the observer never resets the Ring assembler here"）；TimeoutBoundary 走强制链路 `resetAfterPhysicalTimeout()`（`MainWindow.cpp:2469`）→ assembler timeout callback → `sendRingReset()`（`ImagingController.cpp:392-398`）→ svc `ring_reset` → `resetRingRecon()`（`ImagingSvc.cpp:137-141`）。

**补充（后果比原报告写的更重一档）**：跨轮混合产生的那张 snapshot 携带的是**新轮 G+1 的 RoundIdentity**——`sendRingSnapshotToHost` 用的是触发本次快照的 block 的身份。即混合图像不仅产生，还会被打上 G+1 标签进入 G+1 的 PNG 目录与展示链路。修复紧迫性应据此上调。

### 3.2 阻塞点二：snapshot metadata 与实际 SHM pixels 竞态（原报告 §5）——属实

现状代码证据：

- svc 端顺序确为：锁内写 pixels → `h->frame_seq++` → 解锁 →（另起锁读 `frame_seq` 作为 seq）→ 发 `ring_snapshot_ready(seq, submit_index, RoundIdentity)`（`ImagingSvc.cpp:730-741`、`:788-798`，通知构造见 `RingRoundIdentity.h:73-83`）。
- Controller 端 latest-wins 覆盖：`ImagingController.cpp:513-517`，注释原文"处理期间再来新帧：覆盖为最新一帧"。
- worker 复制路径 `lock → memcpy → unlock`（`:600-606`），全程**未读 `h->frame_seq` 与通知 seq 对比**；随后用通知里保存的 `round` 发 UI 信号（`:613-622`）。
- baseline 同样是 latest-wins + 无 seq 校验的 memcpy（baseline `ImagingController.cpp:501`、`:580-585`）。"竞态基线原有、RoundIdentity 使后果升级为 physical-round ownership 错误"的归因准确。

**修复可行性确认**：通知中的 `seq` 就是 SHM header 的 `frame_seq`（`ImagingSvc.cpp:794`），svc 是帧区唯一写者且单调递增。因此原报告 §5.4 的 fail-closed 方案**零协议改动即可实现**：worker 在同一次 `shm->lock()` 内先比对 `h->frame_seq == 通知 seq` 再 memcpy，不等则丢弃并记诊断。

### 3.3 阻塞点三：UI 按 snapshot 数量取模推断 frame end（原报告 §6）——属实

现状代码证据：

- `RingRoundUiState::noteSnapshot` 即 `++snapshotsThisRound_` 后 `% blocksPerFrame == 0` 判 frame end（`RingRoundUiState.cpp:77-87`），baseline 逐字相同（baseline `RingRoundUiState.cpp:53-59`）。
- `completeSnapshot(round)` 仅在取模命中时调用（`MainWindow.cpp:444-455`）；PNG 保存同样 gate 在 `frameEnd`（`MainWindow.cpp:496`）。"exact lookup 的调用时机仍由旧取模决定"属实。
- §6.2 失败示例成立的关键细节已证实：`snapshotsThisRound_` 名字误导，实际**跨轮累积**（仅 TimeoutBoundary/epoch reset 清零，`RingRoundUiState.cpp:109-116`），因此"G 丢 1 张末帧 → G+1 首张凑整误判 frame end"的剧本成立。
- snapshot 可丢失的四条路径全部在码：worker pending 覆盖（`ImagingController.cpp:517`）、双缓冲忙直接丢弃（`:590`，注释"消费端仍在读：丢弃"）、submit_index stale/duplicate 丢弃（`MainWindow.cpp:415-437` + `RingRoundUiState.cpp:40-58`）、ZMQ `dontwait` 非阻塞发送。
- `RingRoundPresentationState` 确为 `map<oldRound, transition>` 精确查找，`currentTarget_` 单调防倒退（`RingRoundPresentation.cpp:46-90`）。§6.3"新 identity state 叠加在旧 callback-count state 上"的定性公允。

### 3.4 共同根因、回退判断与保留清单（原报告 §7、§8、§11）——属实

- assembler identity hard barrier 在码：`RingBlockAssembler.cpp:249-256` 注释明示 "OLD residual can never be completed by [new round identity]"，stale drop（`:155-217`）、超时后保留旧身份地板防迟到数据冒领（`:116-120`）均已实现；baseline assembler 仅有 `triggerSeq` 计数（baseline `RingBlockAssembler.cpp:78-130`），无身份概念。
- 保留清单逐项在码：`RoundIdentity{measurementSession, roundGeneration}`（`include/RoundIdentity.h`）；TriggerGroup 携带身份（`DataTypes.h:46-60` `physicalRoundIdentity()`）；block 回调携带 identity（`MainWindow.cpp:586-589`）；`ring_block_ready` 携带 identity（`ImagingController.cpp:374`）；identity 两字段十进制字符串化、uint64 无损（`RingRoundIdentity.h:41-47`）；`submit_index` stale cutoff（`RingRoundUiState.cpp:118-126`）。
- TimeoutBoundary 链路具备幂等去重（`MainWindow.cpp:2411-2418`）与 ring_reset 失败时 fail-closed 阻塞 admission（`:2473-2486`），实现质量良好。

---

## 4. 原报告需要修正/补充的点

1. **阻塞点一后果评估偏轻**：跨轮混合 snapshot 会被打上**新轮**身份（见 3.1 补充），污染会被错误归属并写入新轮目录。原报告只写到"可能产生跨物理轮混合图像"。

2. **原报告 §9.2 推荐方案不是最优**：原报告建议 svc 维护 `blocksInActiveRound == configuredBlocksPerRound` 计数来产生 `round_complete`。但数据源 `PhysicalRoundNormalizer` 本就产生一次性 `roundComplete` 边界信号（`PhysicalRoundNormalizer.cpp:180`），并经 `FrameConverter` 写入 `TriggerGroup.roundComplete`（`DataTypes.h:46`）——**Ring 链路目前完全丢弃了这个字段**（`RingBlockAssembler` 不读它）。让 svc 再按配置块数计一次数，恰恰是原报告 §7 自己批评的"新增一个计数域"。

3. **§5.4 修复细节需补强**：比对 `frame_seq` 必须与 memcpy 处于**同一次 `shm->lock()` 内**（原文只明确"在锁内复制"），否则留 TOCTOU 缝隙。

4. **一致性备注（不影响结论）**：identity 字段字符串化确实无损；但同一消息中的 `seq`/`submit_index` 仍为 JSON 数字（`qint64`），理论上 >2^53 丢精度。实际不可能达到；若后续统一字符串化可一并处理。

---

## 5. 进一步修改建议（在原报告 §9 基础上）

### 5.1 `round_complete` 主信号改用现成的 `TriggerGroup.roundComplete` 下传

符合原报告 §8.2"业务事实在哪一层真正发生，就由哪一层明确发布"的原则——物理轮完整性的权威源是 Normalizer，不是 svc 计数。

推荐链路：

```text
PhysicalRoundNormalizer          （roundComplete 权威源，已存在）
        │ TriggerGroup.roundComplete（已存在，当前被 Ring 链路丢弃）
        ▼
RingBlockAssembler
        │ 发出的 block 包含 roundComplete 触发组时 → 标记 final block
        ▼
ring_block_ready 携带 final 标记
        ▼
ImagingSvc
        │ activeRound barrier（防跨轮污染，即原报告 §4.4，仍需要）
        │ 收到 final block → 该次 snapshot 置 round_complete=true
        │                  → 随后 reset accumulator
        ▼
Snapshot(seq, submit_index, RoundIdentity, round_complete)
```

边界情形天然 fail-closed：roundComplete 触发组若落在 partial block 里被 assembler 当 residual 丢弃，该轮自然不产生完整快照，不会出现"partial 轮冒充完整轮"。

svc 侧计数（`blocksInActiveRound`）可保留为**交叉校验诊断**（与 Normalizer 信号不一致时报警），不作业务依据。

### 5.2 明确阻塞点三修复后的语义降级方向

改为 `round_complete` 驱动后，末帧 snapshot 若在传输中丢失（latest-wins/双缓冲忙），该轮 PNG/presentation 将**不发生**（fail-closed），替代现在的错误触发（fail-open）。方向正确，但产品侧需知情"丢末帧 = 本轮无最终 PNG"。

可选补偿：`RingRoundPresentationState` 增加 superseded 消费规则——当 admitted snapshot 的 round 大于某 pending transition 的 oldRound 时，旧轮已证终结，可明确消费（或明确跳过并记诊断）该 transition。现状下 `completeSnapshot(G+1)` 对 oldRound=G 的悬挂 transition 只返回 `Missing`，悬挂状态仅 `pendingCount` 诊断可见，最终静默 trim（`RingRoundPresentation.cpp:100-108`）。

### 5.3 可观测性补齐

svc 实现 activeRound barrier 后，G→G+1 触发的 `resetRingRecon()` 应复用现有 `sendRingObservation` 通道发事件（类比 assembler 的 `staleRoundDrops`/`residualTransitions` 计数），否则跨轮 reset 在生产上不可观测，无法与 UI 侧诊断对账。`incoming < activeRound` 的 stale drop 同理记数。

### 5.4 frame_seq 比对实现要点（落实原报告 §5.4）

- worker 在**同一次锁内**完成 `h->frame_seq == 通知 seq` 比对 + memcpy；
- 不等则丢弃该 notification，记录 mismatch 诊断（含通知 seq 与 SHM frame_seq），不触发 PNG/presentation；
- 旧通知被丢弃后，新帧通知会正常匹配，latest-wins 语义保持不变。

### 5.5 测试配套

实现提交已含 `ring_round_identity_test.cpp`、`ring_block_assembler_test.cpp` 等。后续修复建议补：

- svc activeRound barrier 单测（`ring_svc_selftest.cpp` 已有骨架可扩）；
- controller 端 frame_seq 比对单测（需可注入的 SHM mock）；
- "末帧 snapshot 丢失"场景的 UI/presentation 状态机测试——正是当前失败剧本（原报告 §6.2）的回归防线。

---

## 6. 最终审核结论

```text
原报告三个阻塞点归因：属实（均为 baseline 原有缺陷）
原报告"不建议整体回退"：成立
原报告整改大方向：成立

需修正/补充：
  1. 阻塞点一后果应加重：混合 snapshot 被打上新轮身份，污染写入新轮目录
  2. §9.2 round_complete 应改用 TriggerGroup.roundComplete 下传，
     svc 计数降级为交叉校验诊断
  3. §5.4 frame_seq 比对必须与 memcpy 同锁
  4. 补充 round_complete 驱动后"丢末帧=本轮无最终 PNG"的语义说明
     及可选的 superseded 补偿规则
  5. 补齐 svc 侧跨轮 reset/stale drop 的可观测性
```
