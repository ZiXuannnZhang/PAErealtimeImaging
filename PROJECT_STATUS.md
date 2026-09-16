# PAERealtimeImaging 当前项目状态

> 更新时间：2026-09-16（UTC+8）
>
> 本文件是**当前项目状态的单一事实入口**。它描述“现在什么是正式基线、哪些分支仍在验证、下一步做什么”。
> 分支/历史治理以 `REPOSITORY_BASELINE.md` 为准；构建与交付以 `BUILD_STANDARD.md` 为准；具体任务的特殊要求以 `codex/task-docs:TASKS/<task>.md` 为准。

## 1. 当前结论

- `main` 仍是唯一 canonical branch，保存正式源码与正式仓库级文档。
- `codex/task-docs` 仍是任务规格专用分支，不是实现基线。
- 当前最重要的未完成验收是 **PhysicalRoundNormalizer / RoundIdentity 物理轮次归一的实机验收**。
- `codex/physical-round-normalizer-integrated-20260916@52cf7713d7e0e935cb14663ec3470f3a25bfeb90` 已完成软件整改、自动化测试、Windows 构建和真实 ImagingSvc/CUDA selftest；软件侧已知 RoundIdentity 三个阻塞点和 Final Marker 可靠性问题已关闭。
- 但该分支**尚未完成硬件验收，也尚未合并 `main`**。新的控制环境中观察到完整物理轮可能从旧环境约 `4001` 个 trigger 变为约 `4007` 个 trigger，因此此前 `4001 physical -> 1 operational/control + 4000 logical` 的现场假设不能继续当作已证实事实。
- 当前必须先解析最新实机诊断日志，确认完整轮是否稳定为 4007，并验证 CountBoundary / TimeoutBoundary 在新环境中的真实运行证据。日志无证据的项目必须标记为 `UNVERIFIED` / `INCONCLUSIVE`，不能由软件测试结果代替。
- START admission 是**独立工作流**。`codex/start-admission-fence-fix-20260913-003112@6313540f72544c0f68820c4815903abaa0b8c1e1` 的软件修复仍保持 APPROVE，但真实 FPGA/NIC 启动 ingress 问题仍待实机验证；不得与物理轮次归一问题混为一项。

当前状态标签：

```text
ROUND_IDENTITY_CODE_FIXES = IMPLEMENTED
ROUND_IDENTITY_AUTOMATED_TESTS = PASS
ROUND_IDENTITY_WINDOWS_BUILD = PASS
ROUND_IDENTITY_CUDA_SERVICE_SELFTEST = PASS

PHYSICAL_ROUND_NORMALIZATION_HARDWARE_ACCEPTANCE = PENDING
FULL_ROUND_TRIGGER_COUNT_4007 = TO_BE_VERIFIED_FROM_LOCAL_LOGS
COUNT_BOUNDARY_HARDWARE_BEHAVIOR = UNVERIFIED_IN_NEW_4007_ENVIRONMENT
TIMEOUT_BOUNDARY_HARDWARE_BEHAVIOR = UNVERIFIED_IN_NEW_4007_ENVIRONMENT

START_ADMISSION_SOFTWARE = APPROVE
START_ADMISSION_HARDWARE_VALIDATION = PENDING
```

## 2. 当前关键分支

| 角色 | 分支 / SHA | 当前用途 |
|---|---|---|
| 正式基线 | `main` | 唯一 canonical source / docs baseline；正式仓库级文档写入这里 |
| 任务通道 | `codex/task-docs` | `TASKS/` 任务规格与任务通道治理；不是实现基线 |
| Physical round / RoundIdentity 集成候选 | `codex/physical-round-normalizer-integrated-20260916` / `52cf7713d7e0e935cb14663ec3470f3a25bfeb90` | 软件整改与回归已通过；**硬件验收 PENDING**；当前优先解析实机日志 |
| START admission 候选 | `codex/start-admission-fence-fix-20260913-003112` / `6313540f72544c0f68820c4815903abaa0b8c1e1` | 独立启动 ingress-loss 工作流；软件 APPROVE；真实 FPGA/NIC 验证 PENDING |
| Ring pipeline 旧独立方向 | `codex/ring-pipeline-refactor-20260912` | 历史独立未合并方向；不得隐式作为当前主线 |
| backup / historical / experiment | 其他 `backup/*`、旧 `codex/*`、`master` 等 | 仅用于追溯；不得作为新任务默认基线 |

注意：Physical-round integrated 分支与当前 `main` 已经分叉，不能把它描述为 `main` 的简单 fast-forward 延伸，也不能为了“整理历史”直接 rebase/squash/force。任何集成都必须在硬件证据和用户决策之后单独处理。

## 3. PhysicalRoundNormalizer / RoundIdentity 当前验证边界

### 3.1 已完成的软件整改

当前 integrated 候选已经完成并通过软件级验证的核心行为包括：

- RoundIdentity 使用 `(measurementSession, roundGeneration)`，贯穿 Normalizer → Ring assembler → ImagingController → ImagingSvc → snapshot → MainWindow。
- RingBlockAssembler 具备 RoundIdentity hard barrier：新 identity 清旧 residual/phase，旧 identity stale-drop，禁止 block 混合跨轮数据。
- ImagingSvc reconstruction accumulator 具有独立 identity barrier；新 identity 到达前清理旧 reconstruction 状态。
- SHM snapshot metadata 与 pixels 通过 exact sequence + same-lock copy 绑定，避免 notification 与最新 pixels 错配。
- `sourceRoundComplete` 与 `reconstructionComplete` 已拆分；只有 `sourceRoundComplete && blocksConsumed == expectedBlocks` 才允许下游 `round_complete=true`。
- source final 到达但 block 数不匹配时 fail closed：不发布 final completion，记录 `round_block_count_mismatch`，立即关闭/重置旧 reconstruction，防止下一轮补齐旧轮。
- 最终 logical trigger 使用稳定数据属性 `isFinalLogicalTrigger`，避免 one-shot `roundComplete` 被 Ring-disabled card 提前消耗。
- CountBoundary observer 仍保持 one-shot，且不会直接重置 Ring assembler，避免最后一个多卡 trigger 尚未 fan-in 完成时清空数据面。

软件证据已包括：

```text
40/40 CTest PASS
Windows MinGW Debug build PASS
real ImagingSvc + CUDA identity-jump selftest PASS
real ImagingSvc + CUDA dropped-block completion selftest PASS
```

### 3.2 尚未完成的硬件验收

**物理轮次归一整体仍未完成验收。** 当前不能声称：

```text
PhysicalRoundNormalizer completed
RoundIdentity hardware validated
4001 -> 4000 contract confirmed
CountBoundary hardware behavior confirmed
TimeoutBoundary full reset chain confirmed
```

新控制环境已观察到约 `4007 physical triggers / round`。在日志分析完成前，不知道新增触发的实际语义，因此禁止机械修改：

```text
configuredLogicalTriggersPerRound = 4000 -> 4006
```

也禁止未经证据直接把新环境解释成：

```text
4007 physical = 1 control + 4006 logical
```

新增触发可能来自控制器 pre/post trigger、额外 operational event、重复/边界触发、真实 logical scan 变化或统计口径差异；这些目前都只是待验证假设。

## 4. 当前实机日志分析任务

最新大体量诊断包需要在本地 Codex 环境解析。当前第一优先级是建立事实，不是继续修改生产代码。

### 4.1 完整轮 trigger count

必须按 distinct physical trigger identity 重建每个物理轮，至少输出：

```text
measurementSession / candidate round
start/end time
first/last triggerSeq
distinct trigger count
duplicate/gap/wrap
partial/incomplete count
CountBoundary count
TimeoutBoundary count
```

用户已说明：明显因手动暂停造成的短轮不纳入“完整轮是否恒定 4007”的统计。但不能仅以 `<4007` 自动判定为手动暂停，应尽量用 stop/pause、idle、session/boundary 事件和时间间隔佐证；证据不足必须标记“无法判断”。

最终需要回答：

```text
可确认完整轮数量 N
其中 count=4007 的轮数
count!=4007 的异常完整轮
min/max/distribution
triggerSeq gap/duplicate/wrap 情况
```

### 4.2 CountBoundary / 圈末重置

需要从日志重建：

```text
real physical round start
...
CountBoundary timestamp / trigger
roundGeneration before/after
next visible trigger classification
real physical round end
```

重点回答：

- CountBoundary 在哪个 physical trigger 上发生；
- 每个完整 4007-trigger 物理轮中发生几次；
- generation 是否按预期推进；
- 下一可见 trigger 是否被 host 归类为 OperationalStartupControl；
- CountBoundary 是否真实对应物理轮末；
- 是否存在“真实轮尚未结束但 software 已提前 CountBoundary”或“真实轮结束但无 CountBoundary”。

若日志缺少足够前后状态，结论必须写：

```text
无法从当前日志证明 CountBoundary / 圈末重置运行正常
```

### 4.3 TimeoutBoundary / 超时重置

期望通过时间序列证明：

```text
partial physical round
-> idle exceeds timeout
-> PhysicalRoundNormalizer TimeoutBoundary
-> roundGeneration advances
-> Ring residual/reset
-> ring_reset / epoch_reset / resetRingRecon
-> old submit_index / old RoundIdentity stale-drop
-> next RoundIdentity clean start
```

只看到 `timeout_boundary_reset` 计数或单一事件，不足以证明完整链正常。缺失下游 reset、stale-drop 或新轮 clean-start 证据时必须写：

```text
TimeoutBoundary 被记录，但无法证明完整超时重置链正常
```

## 5. START admission 独立工作流

START admission 候选仍为：

```text
codex/start-admission-fence-fix-20260913-003112
6313540f72544c0f68820c4815903abaa0b8c1e1
```

软件侧已确认：

- START 串行发送窗口内按卡 admission race 已修复；
- bounded hold/release、failure/overflow fail-closed；
- `completeStart(false)` no-callback 场景已修复；
- analyzer 使用 decision-first 语义；
- deterministic race/failure/overflow/analyzer/core-network 回归通过。

尚未完成：

- FPGA 收 START 后真实首包/首触发时序；
- NIC/driver/Windows stack 在启动 burst 下的独立丢包；
- 现场触发跳号是否完全由 admission race 解释；
- 应用 stage 1/stage 2/control trace 与 Pktmon/WPR 的真实闭环。

必须保持结论：

```text
START admission 软件竞态已修复并通过软件验收；真实 FPGA/NIC 现场问题仍待实机验证。
```

该工作流不要与 4007 / PhysicalRoundNormalizer 现场行为混为同一根因。

## 6. 当前生产 / 主线架构

`main` 的正式采集架构仍是 PAimage-derived backend：

```text
FPGA cards
  -> Windows UDP
  -> PaimageAcquisition::SocketReceiver
  -> SourceCore
  -> HostOutput / FrameConverter
  -> DataProcessor output interfaces
  -> display / ring realtime / save / publisher
```

旧 `MultiPortReceiver` 仍可存在于兼容和测试路径，但不是当前生产 UDP 入口。

当前采样模型：

```text
sample interval = 4 ns = 250 MSa/s
A + B           = 32 bit/channel = 8 bytes/sample pair
UDP payload     = 1440 bytes (+ 4-byte protocol header)
samples/trigger = acqTimeNs / 4
```

## 7. 当前不应主动重开的区域

除非新的实机日志给出直接证据，否则不要因为 4007 这一新观察就顺手重构已经完成的软件区域：

```text
DataProcessor batch-boundary dequeue
network/source observability
PREPARING/ARMED/RUNNING lifecycle
START admission fence core
Ring SHM exact-seq binding
shared multicard classification
SourceCore packet-assembly timeout isolation
AutoSaveRoundCoordinator mapping
FileSaver roundGeneration rollover
snapshot submit_index stale cutoff
RingReconRoundState identity barrier
CUDA numerical algorithm
FPGA protocol
binary save format
SocketReceiver/UDP core
```

优先证明真实控制环境 trigger pattern，再决定是否需要下一轮源码任务。

## 8. 构建与实机测试规则

仓库级构建规范仍读取最新 `origin/main:BUILD_STANDARD.md`，但**实际构建对象必须是当前任务指定的候选分支和精确 SHA**。

当前存在两个彼此独立的硬件验证对象：

```text
Physical round / RoundIdentity:
  branch = codex/physical-round-normalizer-integrated-20260916
  SHA    = 52cf7713d7e0e935cb14663ec3470f3a25bfeb90

START admission:
  branch = codex/start-admission-fence-fix-20260913-003112
  SHA    = 6313540f72544c0f68820c4815903abaa0b8c1e1
```

不得为了使用最新文档而把 `main` 的源码静默 merge/cherry-pick 进候选；任何源码变化都会使既有软件验收身份失效，需要重新审查。

## 9. 下一步工作顺序

当前优先级：

1. **本地解析最新实机诊断日志**，验证完整物理轮是否稳定为 4007；明确排除有证据的手动暂停短轮。
2. 重建 CountBoundary 时间线，判断圈末重置是否与真实物理轮边界一致。
3. 重建 TimeoutBoundary → Ring/UI/ImagingSvc reset → stale drop → next clean round 证据链；无证据直接标记无法判断。
4. 在上述事实明确前，不修改 `configuredLogicalTriggersPerRound`、first-visible filtering、CountBoundary 语义、FPGA/UDP 协议。
5. 若日志证明当前物理轮归一模型与新控制环境不兼容，再创建独立整改任务。
6. START admission 继续作为独立工作流，在需要时完成真实 FPGA/NIC 验证；不要把其结论用于替代 PhysicalRoundNormalizer 验收。
7. 两条工作流分别达到硬件/system acceptance 后，再由用户决定如何集成回 `main`。

## 10. 文档权威层级

当前操作按以下入口理解：

1. `PROJECT_STATUS.md` — 当前项目、分支、验证状态和下一步。
2. `REPOSITORY_BASELINE.md` — canonical branch、分支角色、历史与 merge 治理。
3. `BUILD_STANDARD.md` — 默认工具链、CMake preset、CUDA/Qt/ZeroMQ、产物与交付规范。
4. `Codex-GitHub双端联动快速上手.md` — ChatGPT/Codex 双端工作流。
5. 当前 `codex/task-docs:TASKS/<task>.md` — 具体任务要求和显式 override。
6. `README.md` 与各子工程 README — 架构与使用入口。
7. `HANDOFF.md` — 兼容仍首先打开 HANDOFF 的旧工作流，只提供指针和当前摘要。

历史 handoff、迁移记录、旧任务报告、旧绝对路径、旧 build cache 和旧候选状态只用于追溯，不得覆盖以上当前文档。

## 11. 本地工作区整理原则

- 先 `git fetch --prune origin`；
- 本地 `main` 只能 `--ff-only` 跟随 `origin/main`；发生分叉必须停止报告；
- 不得为“对齐远端”自动 `reset --hard`、rebase、force push；
- 不得删除被 `.gitignore` 排除但构建所需的本地 CUDA/runtime 依赖、`artifacts/`、build evidence、硬件 captures 或用户数据；
- 保留当前两个硬件验证候选，并分别核对远端 HEAD `52cf7713...` 与 `6313540f...`；
- 历史本地分支仅在确认无独有未推送工作后再清理。
