# Session B 审查追加 — PAimage production 跳号/完整触发丢失统计修复

## 1. Objective

这是 Session B 的 review addendum，只修复首次独立审查发现的一个阻断问题：

> `missingTriggerCount` 和完整 missing-trigger 对应的 `packetsDropped` 当前只在 `DataProcessor::processInputBatch()` 的 legacy/test packet-assembly 路径中累计，而正式 PAimage production ingress 使用 `SocketReceiver -> SourceCore -> HostOutput -> DataProcessor::deliverAssembled()`，不会经过上述 gap 统计，因此现场 `跳号数` 与完整 trigger 丢包当量可能不增长。

本任务要求把 Session B 已冻结的 trigger-gap observability 语义接入**当前 PAimage production 数据路径**，并用 production seam 的 deterministic test 证明。

本任务不是 Session C，不修改 variable-length round、Ring、CUDA、FileSaver timeout 收尾逻辑。

---

## 2. Required preflight

开始前读取：

```powershell
git fetch --prune origin
git show origin/main:PROJECT_STATUS.md
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
git show origin/codex/task-docs:TASKS/SessionB_配置持久化与状态诊断UI_20260917.md
git show origin/codex/task-docs:TASKS/SessionB审查追加_PAimage生产跳号统计修复_20260917.md
git show origin/codex/session-b-ui-observability-20260917:HANDOFF_SESSION_B_20260917.md
git show origin/codex/session-b-ui-observability-20260917:SESSION_B_EXECUTION_RECEIPT_20260917.md
```

重点重新阅读：

```text
MC_410T_MultiCard/delivery/src/PaimageAcquisition/SourceCore.cpp
MC_410T_MultiCard/delivery/include/PaimageAcquisition/SourceCore.h
MC_410T_MultiCard/delivery/src/PaimageAcquisition/NetworkControllerPaimage.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/Backend.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/HostOutput.cpp
MC_410T_MultiCard/delivery/src/DataProcessor.cpp
```

---

## 3. Exact starting point

继续原 Session B implementation branch：

```text
branch             = codex/session-b-ui-observability-20260917
exact starting SHA = 5f6d3ff10779cf93d0da9f2976120cac4fc7f8b9
```

执行：

```powershell
git switch codex/session-b-ui-observability-20260917
git merge --ff-only origin/codex/session-b-ui-observability-20260917
git rev-parse HEAD
```

必须确认 HEAD 精确等于：

`5f6d3ff10779cf93d0da9f2976120cac4fc7f8b9`

若远端已移动，记录差异并停止本 addendum，等待 baseline 重新确认。

不要新建实现分支；不要把 main 源码 merge/rebase/cherry-pick 进来。

---

## 4. Confirmed production-path fact

当前生产启动路径：

```text
NetworkController::start()
  -> startPaimage()
  -> paimage::Backend
  -> SocketReceiver / SourceCore
  -> HostOutput::card()/sync()
  -> DataProcessor::deliverAssembled(...)
```

`HostOutput` 不把生产 packet 再送入：

```text
DataProcessor::enqueuePacket()
DataProcessor::processInputBatch()
```

因此 Session B 已加入 `DataProcessor::processInputBatch()` 的：

```text
missingTriggerCount += skipGap/gap
packetsDropped += gap * expectedPackets
```

虽然对 legacy/test packet path 正确，但不能作为当前 PAimage production gap accounting 的唯一实现。

正式 PAimage stats 当前主要由：

```text
NetworkControllerPaimage.cpp
  receiver().ingressSink
  receiver().observationSink
```

写入 `CardStats`。

---

## 5. Frozen required semantics

Session B 已冻结的用户/统计语义保持不变。

### 5.1 `missingTriggerCount`

含义：

**per-card cumulative count of completely missing triggers inferred from a forward uint16 `triggerSeq` gap.**

示例：

```text
last accepted/closed trigger = T100
next forward trigger         = T104
```

必须：

```text
missingTriggerCount += 3
```

不是 +1 gap event。

### 5.2 `packetsDropped`

现有冻结语义保持：

```text
partial trigger 内缺失 packet
+
完全 missing trigger 的 gap * expectedPackets packet equivalents
```

因此 T100 -> T104、每 trigger 期望 6 包时，完整 missing T101/T102/T103 必须额外贡献：

```text
packetsDropped += 3 * 6
```

### 5.3 Partial + full gap combined

例如 T100 只收到 3/6 包后，下一个 forward trigger 为 T104：

```text
triggersPartial     += 1
packetsDropped      += 3            // T100 内缺包
missingTriggerCount += 3            // T101-T103 整个 trigger 缺失
packetsDropped      += 3 * 6        // T101-T103 包当量
```

不得把 partial T100 本身计入 `missingTriggerCount`。

---

## 6. Required PAimage production implementation

必须在 **PAimage source/observation production seam** 建立完整 trigger-gap accounting。

实现方式可根据现有 SourceCore 架构选择，但必须只有一个明确的 production owner。

### Preferred minimal shape

优先考虑在 `paimage::SourceCore` 的 per-card trigger progression 上产生一个**observation-only** full-trigger-gap 事实，然后由 `NetworkControllerPaimage.cpp::observationSink` 写入既有 `CardStats`：

```text
forward gap quantity = int16_t(newTrigger - previousClosed/acceptedTrigger) - 1
```

若 quantity > 0：

```text
stats.missingTriggerCount += quantity
stats.packetsDropped      += quantity * expectedPackets
```

可以新增一个语义明确的 `paimage::Decision`/Observation event（例如 TriggerGap / MissingTriggerGap），也可以采用等价的小型 per-card SourceCore tracker；命名由实现决定。

关键要求不是特定 enum 名，而是：

1. gap 判定发生在当前 production PAimage source path；
2. 由 SourceCore 已接受的 per-card trigger progression 驱动，不由 UI 推算；
3. NetworkController 的 production `CardStats` 收到准确 gap quantity；
4. 该事件/计数只做 observability，不改变 packet/frame admission、assembly、sync 或 normalization 行为。

---

## 7. Sequence arithmetic / no-false-positive rules

必须保持与既有 DataProcessor gap semantics 一致的 uint16 forward 判断：

```cpp
const int16_t gap = static_cast<int16_t>(newSeq - oldSeq) - 1;
if (gap > 0) { ... }
```

等价实现可以，但 observable behavior 必须一致。

至少满足：

### 7.1 Normal forward gap

```text
T100 -> T104
=> +3
```

### 7.2 Adjacent trigger

```text
T100 -> T101
=> +0
```

### 7.3 uint16 wrap forward

```text
T65534 -> T1
=> missing T65535/T0 => +2
```

### 7.4 Small backward / late trigger

late/backstep packet/trigger 不得制造正 gap。

现有 `RecentTrigger` / stale handling 不得因此改变。

### 7.5 Session/reset boundary

新 measurement session 或 SourceCore 已有明确 assembly/recent reset 后，旧 session trigger anchor 不得用于新 session gap 统计。

不要把跨 session 的 triggerSeq 差值算成 missing triggers。

---

## 8. Exactly-once requirement

同一 per-card trigger transition 的 full gap 必须只累计一次。

特别注意 SourceCore 当前可能产生多个 observation：

```text
TriggerSwitch
Complete / Timeout
Accepted
CardOutput
SyncOutput
```

不要在多个 observation 分支重复计算同一 gap。

推荐让一个 SourceCore-owned transition/event 携带 gap quantity，`NetworkControllerPaimage` 只消费这一事实。

不要同时在 raw `ingressSink` 和 `observationSink` 各自推导同一个 gap。

---

## 9. Existing DataProcessor implementation

Session B 已有 `DataProcessor::processInputBatch()` gap accounting 是 legacy/test path 的正确行为。

本 addendum 默认**保留**它，使旧路径与 production 路径拥有相同统计语义。

不要为了修 production path 删除现有 B1-B4 tests。

如果执行代理发现 production 与 legacy 在某一具体调用模式下会同时处理同一 packet stream，必须用源码证据说明并避免 double count；不要凭假设删除其中一条实现。

---

## 10. Do not change `triggersPartial`

PAimage production 当前：

```text
Decision::TriggerSwitch / Decision::Timeout
=> triggersPartial += 1
=> packetsDropped += expectedPackets - receivedUnique
```

该 partial-trigger 语义保持不变。

新增 full-trigger gap accounting 不能：

- 把完整 missing trigger 加入 `triggersPartial`；
- 把 gap quantity 加入 `triggersDiscarded`；
- 改变 SourceCore `runtimeIncomplete` 的既有定义。

UI 仍保持：

```text
缺失   = triggersPartial
跳号数 = missingTriggerCount
丢包   = packetsDropped
```

---

## 11. Prohibited scope

本 addendum 不得修改：

- Session A `PhysicalRoundNormalizer` 状态机；
- `startupFilterTriggerCount` / `disableCountBoundary` semantics；
- RingConfigDialog / RoundPolicySettings 的已批准 UI/持久化设计（除非仅为编译所需的无语义调整）；
- `已采集 / 已过滤` round display semantics；
- Session C imaging cap；
- timeout screenshot；
- FileSaver timeout round-folder rollover；
- RingBlockAssembler timeout sequencing；
- ImagingSvc reset/finalization；
- CUDA geometry / modulo / block construction；
- `sourceRoundComplete` / `reconstructionComplete` / `expectedBlocks`。

不要借本任务进行无关 SourceCore 重构。

---

## 12. Required production-seam deterministic test

这是本 addendum 的核心验收项。

仅继续跑 `DataProcessor` B1-B4 不足以验收。

必须新增至少一个**真正经过当前 PAimage production source path**的 deterministic test。

### Preferred test seam

优先扩展已有 `network_diagnostics_test` 或建立同等级小型 production test：

```text
NetworkController::start()
  -> startPaimage()
  -> SocketReceiver/SourceCore
```

通过 loopback UDP 向一张卡发送受控 trigger sequence，然后读取：

```text
NetworkController::getCardStats()
```

验证 production `CardStats`。

如果使用更低层 `paimage::Backend` / `SourceCore + production observationSink-equivalent` seam，必须证明测试经过本次真实 production gap owner 和 `CardStats` 写入逻辑，而不是另写一套 fake gap calculator。

---

## 13. Required test scenarios

至少覆盖：

### B-ADD-1 — production full gap

使用简单 `expectedPackets`（例如 1 包/trigger 或明确可计算值）：

```text
完整 T100
完整 T104
```

验证：

```text
missingTriggerCount == 3
packetsDropped == 3 * expectedPackets
triggersPartial == 0
```

### B-ADD-2 — production partial + full gap

例如每 trigger 6 包：

```text
T100 收 3/6
随后 T104 到达
```

验证：

```text
triggersPartial == 1
missingTriggerCount == 3
packetsDropped == 3 + 3*6
```

### B-ADD-3 — adjacent / no gap

```text
T200 -> T201
```

验证 `missingTriggerCount` 不增加。

### B-ADD-4 — wrap forward

```text
T65534 -> T1
```

验证 +2。

### B-ADD-5 — backstep / stale

构造现有 SourceCore 可识别的 late/recent/backward 场景，验证不增加 full missing-trigger count。

### B-ADD-6 — session reset

结束/重开 measurement session 或直接调用对应 production session lifecycle，确保旧 trigger anchor 不污染新 session；新 session 第一 trigger 不产生由旧 session seq 导致的 gap。

---

## 14. Existing regression requirements

保留并重新执行 Session B 已有测试：

```text
data_processor_batch_test
card_status_formatting_test
round_policy_settings_test
network_diagnostics_test
physical_round_normalizer_test
paimage_host_output_test
```

以及完整当前 CTest suite。

原 B1-B4 legacy/DataProcessor gap tests 必须继续 PASS。

这样可以同时证明：

```text
legacy/test DataProcessor path semantics = unchanged
PAimage production path semantics        = now correct
```

---

## 15. Runtime diagnostics

`CardStats::missingTriggerCount` 已进入：

```text
NetworkController::runtimeStatsFields()
```

该 JSON mapping 应保持。

如果新增 SourceCore gap Decision，建议在现有 trace/observation 体系中保留足够可诊断字段（至少 card、当前 trigger、gap quantity），但不要为本 addendum 扩张新的大型 diagnostic schema。

---

## 16. Expected changed files

代码修改预计主要集中于：

```text
MC_410T_MultiCard/delivery/include/PaimageAcquisition/SourceCore.h
MC_410T_MultiCard/delivery/src/PaimageAcquisition/SourceCore.cpp
MC_410T_MultiCard/delivery/src/PaimageAcquisition/NetworkControllerPaimage.cpp
```

以及 production-seam test，例如：

```text
MC_410T_MultiCard/delivery/tests/network_diagnostics_test.cpp
```

必要时调整测试 CMake。

不要求机械修改所有文件。

若生产代码 diff 显著扩展到 UI/Ring/FileSaver/CUDA，说明已经越出本 addendum 范围，应停止扩张并记录 blocker。

---

## 17. Windows build / tests

在最终代码 commit 上按最新：

`origin/main:BUILD_STANDARD.md`

重新执行：

1. test configure/build；
2. 完整 CTest；
3. 本 addendum focal production-gap test；
4. 标准 Windows full build：

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging\MC_410T_MultiCard\delivery"
cmd /c build_mingw_debug.cmd
```

记录真实命令、exit/result 和 exact code SHA。

本任务不修改 CUDA core，不要求重新生成 CUDA runtime；依赖 receipt 仍按 BUILD_STANDARD 适用范围记录。

---

## 18. Required review-addendum receipt

新增：

`SESSION_B_REVIEW_ADDENDUM_RECEIPT_20260917.md`

至少记录：

### Git identity

```text
branch
starting SHA = 5f6d3ff10779cf93d0da9f2976120cac4fc7f8b9
fix code commit
final remote HEAD（最终回复 out-of-band 可避免自引用）
tracked git status
```

### Root cause

明确记录：

```text
原 missingTriggerCount 只在 DataProcessor packet-assembly path 增量；
production PAimage path 经 SourceCore/HostOutput/deliverAssembled 绕过该逻辑。
```

### Final production owner

写明最终真实实现：

- 哪个 SourceCore state/event 计算 gap；
- 哪个 Observation/Decision 携带 gap quantity；
- `NetworkControllerPaimage` 哪个分支写 `missingTriggerCount`；
- `packetsDropped` 如何加入 full-trigger equivalents；
- 如何避免 double count。

### Exact tests/build

逐条记录真实命令与 PASS/FAIL/NOT RUN。

---

## 19. Update Session B handoff

最小更新：

`HANDOFF_SESSION_B_20260917.md`

修正其中关于 `missingTriggerCount` production owner 的描述。

Handoff 必须区分：

```text
DataProcessor legacy/test path
PAimage production SourceCore path
```

并告诉 Session C：统计语义已经在 production PAimage ingress 完成，不需要 Session C 再实现一遍。

不要改写 Session B 已批准的 UI/配置部分。

---

## 20. Acceptance criteria

本 addendum 只有全部满足才可关闭 Session B：

1. PAimage production source path 能识别 forward triggerSeq full gaps；
2. T100 -> T104 在 production seam 精确 `missingTriggerCount += 3`；
3. 同场景 `packetsDropped += 3 * expectedPackets`；
4. partial + full gap 可同时准确累计且不互相混淆；
5. adjacent trigger 不增加 gap count；
6. uint16 wrap-forward 正确；
7. late/backstep/recent trigger 不制造 false gap；
8. measurement session reset 清除/隔离 gap anchor；
9. 同一 transition exactly once，不双计；
10. 不改变 packet/frame admission、SourceCore assembly/sync、Normalizer、Ring/CUDA/FileSaver 行为；
11. 原 DataProcessor B1-B4 tests 继续 PASS；
12. 新 production-seam tests PASS；
13. 完整 CTest PASS；
14. Windows full build PASS；
15. receipt + HANDOFF 更新已 push；
16. local HEAD == remote branch HEAD；
17. hardware validation 仍为 PENDING。

---

## 21. Required final execution report

Codex 完成后报告：

```text
Task document
Branch = codex/session-b-ui-observability-20260917
Starting SHA = 5f6d3ff10779cf93d0da9f2976120cac4fc7f8b9
Fix code commit(s)
Final remote HEAD
Changed files
Root cause confirmed
Final PAimage production gap owner / event flow
T100->T104 production test result
Partial+gap production test result
Wrap/backstep/session-reset results
Full CTest command + result
Windows build command + result
SESSION_B_REVIEW_ADDENDUM_RECEIPT_20260917.md
Updated HANDOFF_SESSION_B_20260917.md
Tracked git status
Local HEAD == remote HEAD
Hardware validation = PENDING
```

---

## 22. Validation boundary

即使本 addendum 全部通过，也只表示：

```text
SESSION_B_SOFTWARE_IMPLEMENTATION = PASS
SESSION_B_PAIMAGE_PRODUCTION_GAP_ACCOUNTING = PASS
SESSION_B_AUTOMATED_TESTS = PASS
SESSION_B_WINDOWS_BUILD = PASS
```

仍保持：

```text
PHYSICAL_ROUND_HARDWARE_VALIDATION = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS = NOT PROVEN BY THIS TASK
```

不要把 loopback/自动测试结果描述为真实 FPGA/NIC/LabVIEW 实机验证。
