# Session A — PhysicalRoundNormalizer 核心轮次策略扩展

## 1. Objective

本任务是四阶段串行改造中的 Session A。

目标：只建立底层物理轮次策略能力，为后续独立 Session B/C/D 提供稳定 production API 与 round metadata。

本阶段实现：

- 可配置 startup trigger 过滤数量；
- 可独立关闭 fixed-count CountBoundary；
- current / last-completed round 级 physical/filtered 观测字段；
- HostOutput production 配置入口；
- 对应 deterministic tests 与回归验证。

本阶段不实现 UI、跳号统计、FileSaver/Ring/CUDA 新生产行为。

---

## 2. Required preflight

开始前读取最新仓库治理与本任务：

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging"
git fetch --prune origin
git show origin/main:PROJECT_STATUS.md
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
git show origin/codex/task-docs:TASKS/PhysicalRoundNormalizer核心策略扩展_SessionA_20260917-114452.md
```

根 `HANDOFF.md` 与历史 handoff/report 仅用于追溯，不覆盖上述当前文档和本 task。

---

## 3. Baseline / exact starting SHA

本任务对普通“新任务从 main 开始”规则做显式 baseline override。

必须从当前 PhysicalRoundNormalizer / RoundIdentity 集成候选开始：

```text
baseline branch = codex/physical-round-normalizer-integrated-20260916
starting SHA    = 52cf7713d7e0e935cb14663ec3470f3a25bfeb90
```

该分支/SHA 已完成软件整改与软件级验证，但硬件验收仍 PENDING。

不要直接继续提交到该保留候选；从精确 starting SHA 创建独立实现分支。

开始时核对：

```powershell
git switch codex/physical-round-normalizer-integrated-20260916
git merge --ff-only origin/codex/physical-round-normalizer-integrated-20260916
git rev-parse HEAD
```

必须确认 HEAD 为：

`52cf7713d7e0e935cb14663ec3470f3a25bfeb90`

如果远端保留候选已移动，与本任务指定 SHA 不一致，则记录差异并停止本任务实现，等待新的 baseline 决策。

---

## 4. Target implementation branch

建议分支：

`codex/session-a-round-policy-core-20260917-114452`

实现分支必须从上述 exact starting SHA 创建。

仓库分支要求：

- `codex/task-docs` 仅存任务规格，不承载实现源码；
- 不直接在 `main` 实施；
- 不改写保留候选分支；
- 不把最新 `main` 源码 merge/rebase/cherry-pick 到本实现分支；
- 不改写共享历史。

---

## 5. Current architecture / facts to preserve

修改前至少阅读：

- `MC_410T_MultiCard/delivery/include/PaimageAcquisition/PhysicalRoundNormalizer.h`
- `MC_410T_MultiCard/delivery/src/PaimageAcquisition/PhysicalRoundNormalizer.cpp`
- `MC_410T_MultiCard/delivery/include/PaimageAcquisition/HostOutput.h`
- `MC_410T_MultiCard/delivery/src/PaimageAcquisition/HostOutput.cpp`
- `RoundIdentity.h`
- Normalizer / HostOutput / production-blocker 相关测试与 CMake test registration

需要保持的现有语义：

1. `PhysicalRoundNormalizer` 是多卡共享的物理轮次归一化器。
2. 同一 `(measurementSession, triggerSeq)` 的多卡数据共享一次 classification。
3. recent decision cache 用于让 late card 复用旧 classification。
4. cached hit 不重复计 `newDistinct`，不重复产生 one-shot `roundComplete`；stable `isFinalLogicalTrigger` 保持原 classification 属性。
5. `RoundIdentity = (measurementSession, roundGeneration)`。
6. physical-round timeout 基于 physical idle；SourceCore 单卡 packet assembly timeout 不等价于 physical-round boundary。
7. Ring / Imaging / FileSaver 已有 RoundIdentity hard barrier、fail-closed 语义不在本任务中调整。
8. 当前 `OperationalStartupControl` 是 host-side operational classification，不是已由 FPGA/LabVIEW 协议证明的字段。

---

## 6. Core configuration: `startupFilterTriggerCount`

新增核心配置：

`startupFilterTriggerCount`

推荐类型：与现有计数体系兼容的无符号整数，例如 `std::uint64_t`。

底层默认值：

`1`

Session A 只实现核心状态、默认值和 production API；UI/QSettings/“设为默认”属于 Session B。

### 6.1 Exact semantics

每个新的 physical round 开始后，过滤前：

`X = startupFilterTriggerCount`

个 **new distinct physical trigger identities**。

计数单位不是 packet、card frame、card 数或 sync frame。

同一 physical trigger 的多卡 classification 只能消耗一个 filter slot。

### 6.2 X=0

当：

`startupFilterTriggerCount = 0`

新 round 不过滤 trigger。

第一枚 new distinct physical trigger：

```text
decision = LogicalScan
logicalTriggerIndex = 0
```

### 6.3 X=1

默认兼容模式：

- 第一枚 new distinct trigger filtered；
- 第二枚 new distinct trigger = logical #0。

### 6.4 X=N

例如 X=7：

```text
physical distinct #1..#7 -> filtered
physical distinct #8     -> LogicalScan #0
physical distinct #9     -> LogicalScan #1
...
```

---

## 7. Multicard / cache semantics

例如 triggerSeq=100 来自多张卡：

第一次 classification：

- `newDistinct=true`；
- current physical distinct +1；
- 如果仍在 startup filtering，则消耗一个 filter slot。

后续同 trigger cache hit：

- `newDistinct=false`；
- 复用相同 decision/index/generation；
- current physical distinct 不增加；
- current filtered 不增加；
- 不再消耗 filter slot。

late card 同理。

---

## 8. Partial-startup timeout

必须支持：

```text
startupFilterTriggerCount = 7
当前轮只观察到 3 个 startup distinct triggers
随后 physical idle 超过 timeout
```

这一轮已经开始，因此 explicit `timeoutBoundary()` 必须能够结束它：

- latch 本轮 physical / filtered counts；
- 清零 current per-round counters；
- `roundGeneration++`；
- 下一轮重新从 startup filter slot 1 开始。

换言之，只要当前轮已经观察到至少一个 new distinct physical trigger，就存在可由 timeout 关闭的 active physical round。

---

## 9. Core configuration: `disableCountBoundary`

新增核心配置：

`disableCountBoundary`

类型：`bool`

底层默认值：

`false`

Session A 只实现核心语义与 production API；前端 checkbox 属于 Session B。

### 9.1 `disableCountBoundary == false`

保持当前 CountBoundary 行为。

当：

`currentLogicalDistinctCount == configuredLogicalTriggersPerRound`

继续执行现有语义，包括：

- 当前 trigger 为最后一个 logical trigger；
- `roundComplete=true` one-shot；
- `isFinalLogicalTrigger=true` stable property；
- `countBoundaryResets++`；
- current logical count 清零；
- `roundGeneration++`；
- 下一 physical round 重新执行 startup filtering。

### 9.2 `disableCountBoundary == true`

达到 configured logical count 时，不产生 fixed-count round boundary。

不得发生：

- `roundComplete=true`；
- `isFinalLogicalTrigger=true`；
- `CountBoundary` event；
- `countBoundaryResets++`；
- current logical count 清零；
- `roundGeneration++`；
- 重新进入 startup filtering。

logicalTriggerIndex 必须继续单调增加。

例如 configured=4000：

```text
logical indices = 0 ... 3999, 4000, 4001, 4002 ...
RoundIdentity   = 保持同一轮
```

直到 TimeoutBoundary。

Session A 不实现“超过 configured count 后停止实时成像”；该行为属于 Session C。

---

## 10. TimeoutBoundary semantics

无论 `disableCountBoundary` 为 false/true，TimeoutBoundary 始终有效。

不要使用：

`logicalTriggersPerRound = 0`

表达 timeout-only 模式。`logicalTriggersPerRound` 继续保持正数。

TimeoutBoundary 后：

- latch 当前轮 per-round counters；
- current per-round counters 清零；
- current logical count 清零；
- `roundGeneration++`；
- 下一轮重新执行 startup filtering；
- 下一轮 logical index 从 0 开始；
- cache / late-card 原有防重复与旧 classification 复用语义保持。

本任务不调整 Ring/CUDA/FileSaver timeout 行为。

---

## 11. New shared per-round observation counters

这些字段由 `PhysicalRoundNormalizer` 统一维护，供后续 Session B UI 使用。

至少新增：

### `currentPhysicalDistinctCount`

当前 physical round 已观察到的 new distinct physical trigger 数。

包含 startup-filtered trigger 与 LogicalScan trigger；不包含 cache/late-card 重复 classification。

### `currentStartupFilteredCount`

当前 physical round 实际已经过滤的 startup physical trigger 数。

### `lastCompletedPhysicalDistinctCount`

最近一次 CountBoundary 或 TimeoutBoundary 结束轮的最终 physical distinct count。

### `lastCompletedStartupFilteredCount`

最近一次 CountBoundary 或 TimeoutBoundary 结束轮的最终 startup filtered count。

### Boundary ordering

CountBoundary / TimeoutBoundary 都应：

1. 先 latch 即将结束轮的 counts；
2. 再清零 current counters；
3. 开始下一轮状态。

partial-startup timeout 也应正确 latch，例如 X=7、只观察 3 个 filtered triggers：

```text
lastCompletedPhysicalDistinctCount = 3
lastCompletedStartupFilteredCount  = 3
```

### New measurement session

`beginSession()` 后：

- current per-round counters = 0；
- last-completed per-round counters = 0；
- startup filtering 从新 session 第一轮重新开始；
- 旧 session cache/round state 不污染新 session。

---

## 12. Preserve cumulative statistics

保留现有累计统计语义，包括：

- `physicalDistinctObserved`
- `operationalControlFiltered`
- `logicalDistinctAccepted`
- `countBoundaryResets`
- `timeoutBoundaryResets`

current-round counters 不替代这些累计字段。

如无必要，不做大范围 rename。

---

## 13. Production API requirement

Session A 必须通过 production chain 暴露后续 Session B/C 可消费的配置入口，使后续代码不需要直接访问 Normalizer 私有状态。

推荐能力：

```text
setStartupFilterTriggerCount(...)
setDisableCountBoundary(...)
```

具体函数签名可以遵循现有：

- `setConfiguredLogicalTriggersPerRound`
- `setPhysicalRoundTimeout`

的接口风格。

最终真实 API 名称、参数类型、Snapshot 字段名必须写入 HANDOFF。

默认行为必须保持：

```text
startupFilterTriggerCount = 1
disableCountBoundary      = false
```

---

## 14. Runtime configuration boundary

用户生产操作会避免采集中修改这些参数。

因此本任务不要求增加新的 mid-round transactional reconfiguration 机制。

要求：

- production API 行为有明确状态语义；
- 注释说明这些参数按 measurement-session/configuration 边界使用；
- automated tests 不依赖采集中任意切换参数。

如现有 configuration-restart 机制已经覆盖相关调用，可复用现有路径。

---

## 15. Invariants

实现后继续满足：

1. measurementSession / RoundIdentity 现有有效性约束。
2. RoundIdentity 顺序语义不改变。
3. 同一 physical trigger 的多卡只产生一个 shared decision。
4. late-card cache hit 不重复 boundary、不重复 per-round 统计、不消耗新轮 filter slot。
5. CountBoundary one-shot `roundComplete` 与 stable `isFinalLogicalTrigger` 的分离继续成立。
6. SourceCore packet-assembly timeout 不作为 physical-round idle timeout。
7. recent decision cache 继续有界。
8. new session 不继承旧 session 的 current/last round state。
9. timeout 后旧 round late frame 继续复用其旧 cached classification，不计入新轮。
10. 默认 `X=1 + disableCountBoundary=false` 与 starting SHA 的生产行为兼容。
11. AutoSaveRoundCoordinator、FileSaver、Ring、ImagingSvc 现有行为不在本 Session 中重定义。

---

## 16. Prohibited Scope

若实现需要修改下列范围才能继续，应在执行报告中列为 blocker，不在 Session A 中扩展需求。

### Session B scope

不实现：

- “启动过滤触发数”UI；
- “禁用计数重置”UI；
- QSettings / “设为默认”；
- `missingTriggerCount` / “跳号数”；
- “报文不完整触发”→“缺失”展示改名；
- “已采集 / 已过滤”展示；
- tooltip 调整。

### Session C scope

不实现：

- configured count 后停止 sync/display/Ring/CUDA；
- imaging cap；
- timeout 保存当前窗口图；
- timeout 新的 save-folder production integration；
- Ring timeout reset 新策略；
- ImagingSvc timeout reset 新策略；
- partial Ring block；
- CUDA block/算法修改；
- angle modulo 修改。

### Completion protocol

不修改：

- `sourceRoundComplete`
- `reconstructionComplete`
- `expectedBlocks`
- 完整圈 final PNG 判定

不为可变长度 timeout round 回溯或伪造 final logical trigger。

### Repository-level governance

本 implementation branch 不修改：

- `PROJECT_STATUS.md`
- `REPOSITORY_BASELINE.md`
- `BUILD_STANDARD.md`
- `codex/task-docs`

---

## 17. Implementation Freedom

执行代理可以在保持本任务 observable semantics、既有 invariants 与 Prohibited Scope 的前提下选择具体实现方式，包括：

- 是否保留 `AwaitingControl / CollectingScan` enum；
- 是否增加内部 startup-filter progress 字段；
- setter 与 constructor 的组合；
- Snapshot 字段布局；
- test helper 组织方式。

优先维持单一 round-state source，避免建立重复状态机。

---

## 18. Expected Changed Files

预计主要涉及：

- `MC_410T_MultiCard/delivery/include/PaimageAcquisition/PhysicalRoundNormalizer.h`
- `MC_410T_MultiCard/delivery/src/PaimageAcquisition/PhysicalRoundNormalizer.cpp`
- `MC_410T_MultiCard/delivery/include/PaimageAcquisition/HostOutput.h`
- 必要时 `MC_410T_MultiCard/delivery/src/PaimageAcquisition/HostOutput.cpp`
- `physical_round_normalizer_test` 相关测试源
- `paimage_host_output_test` 相关测试源
- 必要的 test CMake registration

若实际修改文件明显超出该范围，在执行报告中解释依赖关系。

---

## 19. Required deterministic tests

测试应覆盖 production `PhysicalRoundNormalizer` / `HostOutput` seam。

至少覆盖：

### A1 — default compatibility

```text
X=1
disableCountBoundary=false
```

验证 starting SHA 默认行为保持。

### A2 — X=0

第一枚 distinct：

```text
LogicalScan
logical index 0
filtered current count 0
```

达到 configured count 后正常 CountBoundary。

### A3 — X=7

前 7 个 new distinct filtered，第 8 个 LogicalScan #0。

### A4 — multicard duplicate identity

同一 trigger 多次 classify：

- current physical +1 only；
- current filtered +1 at most；
- logical count +1 at most；
- cached classification `newDistinct=false`。

### A5 — late card across boundary

boundary 前已分类 trigger 的 late card 在 boundary 后到达：

- 复用旧 classification；
- 不消耗新 round startup filter slot；
- 不增加新 round current physical/filtered；
- 不制造重复 boundary。

### A6 — X=0 + CountBoundary enabled

第一 trigger logical #0，达到 configured count 正常 CountBoundary。

### A7 — X=7 + CountBoundary enabled

达到 configured logical count：

- CountBoundary；
- generation 前进；
- 下一轮重新过滤 7 个。

### A8 — disableCountBoundary=true

使用较小 configured count，例如 4，验证：

```text
logical indices = 0,1,2,3,4,5...
```

超过 configured count 后：

- 无 CountBoundary；
- generation 不变；
- `roundComplete=false`；
- `isFinalLogicalTrigger=false`；
- `countBoundaryResets` 不增加。

### A9 — disableCountBoundary + timeout

超过 configured count 后触发 timeout：

- TimeoutBoundary；
- lastCompleted counts 正确；
- current counts 清零；
- generation +1；
- 下一轮重新执行 startup filtering。

### A10 — partial startup timeout

X=7，只观察 3 个 startup distinct triggers 后 timeout：

```text
lastCompletedPhysicalDistinctCount = 3
lastCompletedStartupFilteredCount  = 3
```

新轮重新从第一个 startup filter slot 开始。

### A11 — per-round vs cumulative counters

验证 current、last-completed、cumulative 三组字段语义互不混淆。

### A12 — session reset

新 measurement session：

- current / last-completed per-round counters reset；
- startup filtering 重新开始；
- old session cache/identity 不污染新 session。

---

## 20. Regression Baseline

至少运行当前树中实际存在的：

- `physical_round_normalizer_test`
- `paimage_host_output_test`

并运行直接相关的 round / host / production-blocker tests。

如果实际 target 名不同，以当前 CMake target 为准，并在执行报告中记录准确命令。

---

## 21. Build / Test standard

构建与测试遵循最新：

`origin/main:BUILD_STANDARD.md`

但实际构建对象必须是 Session A implementation branch 的最终 commit。

不得为了使用最新构建规范而把 main 源码合入实现分支。

如果标准 Windows 工具链可用，完成：

- final commit 上重新 configure；
- canonical `build_mingw_debug.cmd` / 对应 mingw-debug build；
- task-required CTest/regression。

本任务未修改 CUDA 核心，因此 CUDA service selftest 不是 Session A 的必选验收项；如实际执行，可单独记录结果。

如果某构建/测试未执行，执行报告中记录 `NOT RUN` 和具体原因，不将其记为 PASS。

---

## 22. Acceptance Criteria

Session A 软件实施完成需要同时满足：

1. `startupFilterTriggerCount` core 已实现；
2. X=0/1/N tests 通过；
3. multicard/cache/late-card shared-decision 语义保持；
4. `disableCountBoundary=false` 默认模式兼容；
5. `disableCountBoundary=true` 时 logical index 可越过 configured count；
6. 越过 configured count 不产生 CountBoundary/final marker/generation reset；
7. TimeoutBoundary 在两种 CountBoundary 配置下仍推进 generation；
8. partial-startup timeout 能结束已开始轮次；
9. current / last-completed round physical/filtered counters 正确；
10. cumulative counters 保持原语义；
11. HostOutput 暴露后续 Session B/C 可消费的 production API；
12. required old/new tests 完成并记录结果；
13. Windows build 完成并记录结果，或明确记录 NOT RUN 原因；
14. implementation commit 已推送；
15. local HEAD == remote implementation branch HEAD；
16. tracked working tree clean；
17. Session A HANDOFF 已生成并提交。

---

## 23. Required HANDOFF for Session B

在 implementation branch 中生成：

`HANDOFF_SESSION_A_20260917.md`

至少包含：

### Repository state

- repository
- implementation branch
- baseline branch
- exact starting SHA
- implementation commit SHA
- final remote HEAD
- git status

### Implemented scope

准确列出：

- startup filter count
- disable CountBoundary
- new per-round counters
- production setters/API

### Exact semantics

记录：

- X=0 / X=1 / X=N
- CountBoundary enabled/disabled
- TimeoutBoundary
- partial-startup timeout
- cached/late-card behavior

### Modified files

逐项列出。

### Production interfaces for next sessions

必须写最终真实函数名、参数类型、字段名和 Snapshot 字段名。

### Frozen invariants

记录 Session B/C 不应改变的既有语义。

### Tests / Build

逐项记录实际命令与：

- PASS
- FAIL
- NOT RUN

以及必要说明。

### Not implemented

明确记录本 Session 未实现：

- UI / QSettings / 设为默认
- missingTriggerCount / 跳号数
- label/tooltip 改造
- imaging cap
- timeout screenshot
- timeout save-folder production integration
- Ring/CUDA behavior changes

### Hardware/system validation boundary

记录：

`HARDWARE_VALIDATION_PENDING`

Session A 的软件结果不构成真实 FPGA/LabVIEW 触发语义或物理轮行为的硬件验收。

### Instructions for Session B

记录：

- Session B 应从哪个 branch/final HEAD 开始；
- 可直接消费哪些新增 production API；
- Normalizer 已冻结的行为语义。

不创建自引用 commit 仅为把 handoff 自己的最终 SHA 写回 handoff；最终 remote HEAD 可在执行回执中报告。

---

## 24. Required Execution Report

完成后报告：

```text
Task document
Implementation branch
Baseline branch / starting SHA
Implementation commit
Final remote HEAD
Changed files
Exact production API added
Tests: commands + results
Windows configure/build: commands + results
Any NOT RUN items and reasons
HANDOFF path
Tracked git status
Local HEAD == remote HEAD receipt
Hardware validation = PENDING
```

报告应给出具体命令和结果，不用笼统的“测试已通过”替代证据。

---

## 25. Hardware / system validation boundary

本 Session 只建立软件策略能力。

只有实际执行并成功的项目才能标记为 PASS，例如：

```text
SESSION_A_SOFTWARE_IMPLEMENTATION = PASS
SESSION_A_AUTOMATED_TESTS = PASS
SESSION_A_WINDOWS_BUILD = PASS
```

真实设备验证状态仍为：

```text
PHYSICAL_ROUND_HARDWARE_VALIDATION = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS = NOT_PROVEN_BY_SESSION_A
```
