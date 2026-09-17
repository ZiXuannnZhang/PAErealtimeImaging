# Session B 二次审查追加 — 同 Session triggerSeq 复位恢复与 HANDOFF 收口

## 1. Objective

这是 Session B 的第二次 review addendum，只处理最终审查剩余的两个问题：

1. PAimage production `SourceCore` 的 full-trigger-gap tracker 当前能处理 forward/wrap、小回退和 measurement-session reset，但**同一 measurement session 内发生大幅 triggerSeq 回退/复位后，gap anchor 不会重新建立**，导致后续 `missingTriggerCount` / full-trigger `packetsDropped` 长时间失明；
2. `HANDOFF_SESSION_B_20260917.md` 的 Session C 输入仍引用 Session B 初始代码 commit / 41/41 测试摘要，未反映第一次 review addendum 后的真实最终分支状态。

本任务只做上述收口，不重开 Session B 的 UI/配置/统计设计，不进入 Session C。

---

## 2. Required preflight

开始前读取：

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging"
git fetch --prune origin
git show origin/main:PROJECT_STATUS.md
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
git show origin/codex/task-docs:TASKS/SessionB_配置持久化与状态诊断UI_20260917.md
git show origin/codex/task-docs:TASKS/SessionB审查追加_PAimage生产跳号统计修复_20260917.md
git show origin/codex/task-docs:TASKS/SessionB二次审查追加_同Session触发序号复位恢复_20260917-211159.md
git show origin/codex/session-b-ui-observability-20260917:HANDOFF_SESSION_B_20260917.md
git show origin/codex/session-b-ui-observability-20260917:SESSION_B_REVIEW_ADDENDUM_RECEIPT_20260917.md
```

重点重新阅读：

```text
MC_410T_MultiCard/delivery/include/DataProcessor.h
MC_410T_MultiCard/delivery/src/DataProcessor.cpp
MC_410T_MultiCard/delivery/include/PaimageAcquisition/SourceCore.h
MC_410T_MultiCard/delivery/src/PaimageAcquisition/SourceCore.cpp
MC_410T_MultiCard/delivery/tests/paimage_production_gap_test.cpp
```

---

## 3. Exact starting point

继续原 Session B implementation branch：

```text
branch             = codex/session-b-ui-observability-20260917
exact starting SHA = 2566a028a590ec2e099a12dd00977f704c5e38e0
```

执行：

```powershell
git switch codex/session-b-ui-observability-20260917
git merge --ff-only origin/codex/session-b-ui-observability-20260917
git rev-parse HEAD
```

必须确认 HEAD 精确等于：

`2566a028a590ec2e099a12dd00977f704c5e38e0`

若远端已移动，记录差异并停止本 addendum，等待 baseline 重新确认。

不要新建实现分支，不要把 main 源码 merge/rebase/cherry-pick 进来。

---

# Part I — 同 Session triggerSeq reset recovery

## 4. Confirmed current behavior

第一次 review addendum 已建立 production owner：

```text
SourceCore::ingest()
  fresh assembly activation
  -> per-card gapAnchor / gapAnchorValid
  -> Decision::TriggerGap(count = completely missing triggers)
  -> NetworkControllerPaimage::observationSink
  -> CardStats::missingTriggerCount
  -> CardStats::packetsDropped += count * expectedPackets
```

当前核心逻辑等价于：

```cpp
const auto delta = static_cast<std::int16_t>(trigger - gapAnchor);
if (delta > 1)
    emit TriggerGap(delta - 1);
if (delta > 0)
    gapAnchor = trigger;
```

这已经正确覆盖：

- normal forward gap；
- adjacent trigger；
- uint16 wrap forward；
- small backward / late trigger 不制造假 gap；
- measurement session 重新 start 时清 anchor。

但对于**同一 measurement session 内的大幅 triggerSeq 回退/源计数器复位**，`delta <= 0` 后 anchor 永远不移动。

例如：

```text
old gapAnchor = T1000
source trigger counter resets -> T10
then T14
```

当前行为：

```text
T10: 不计 gap，也不重新建立 anchor；anchor 仍为 T1000
T14: 仍相对 T1000 为 backward；不计 gap
```

于是复位后真正缺失的 T11/T12/T13 无法统计。

---

## 5. Required reset-recovery semantics

production gap tracker 必须与现有 DataProcessor 的冻结 reset-recovery 口径一致。

`DataProcessor` 当前明确使用：

```cpp
static constexpr int kTriggerResetBackJumpThreshold = 256;
```

含义：当新 trigger 相对旧 anchor 是**大幅直接回退 >= 256** 时，视为 trigger counter / new-frame-style reset recovery，重新建立统计 anchor；该 reset transition 本身不记 full-trigger gap。

SourceCore production gap tracker 也必须使用同一数值语义：

```text
reset back-jump threshold = 256
```

无需为了本任务在 DataProcessor 与 SourceCore 间建立新的架构依赖；可以在 SourceCore 使用命名清晰的同值常量并注释与 DataProcessor frozen semantics 对齐。

---

## 6. Required sequence decision order

必须先保持现有 signed uint16 wrap-aware forward 判断，再判断 direct large back-jump；避免把正常 wrap forward 错判为 reset。

建议 observable logic 等价于：

```cpp
const std::int16_t delta = static_cast<std::int16_t>(trigger - gapAnchor);

if (delta > 0) {
    if (delta > 1)
        emit TriggerGap(delta - 1);
    gapAnchor = trigger;
} else {
    const std::int32_t directBackJump =
        static_cast<std::int32_t>(gapAnchor) -
        static_cast<std::int32_t>(trigger);

    if (directBackJump >= 256) {
        // same-session counter reset recovery
        gapAnchor = trigger;
        // no TriggerGap for the reset transition itself
    }
    // otherwise small late/backstep: no gap and do not move anchor
}
```

具体代码结构可以不同，但必须满足下面的 observable contract。

---

## 7. Frozen observable contract

### 7.1 Normal forward gap remains unchanged

```text
T100 -> T104
=> missingTriggerCount += 3
=> packetsDropped += 3 * expectedPackets
```

### 7.2 Normal uint16 wrap remains forward progression

```text
T65534 -> T1
=> delta = +3 in signed uint16 progression
=> missing T65535/T0 = +2
```

不得因为直接数值看起来 `65534 > 1` 而当作 reset。

### 7.3 Small backstep remains late/backward

```text
anchor T105 -> observed T102
backJump = 3 < 256
=> no gap
=> anchor remains T105
```

后续 T106 必须继续被视为 T105 的 adjacent forward trigger。

### 7.4 Large same-session reset

```text
anchor T1000 -> observed T10
backJump = 990 >= 256
```

必须：

```text
reset transition adds 0 to missingTriggerCount
reset transition adds 0 full-trigger equivalents to packetsDropped
new gapAnchor = T10
```

随后：

```text
T10 -> T14
=> missingTriggerCount += 3
=> packetsDropped += 3 * expectedPackets
```

### 7.5 Threshold boundary

冻结阈值边界：

```text
backJump 255 => small backstep, do not re-anchor
backJump 256 => reset recovery, re-anchor
```

### 7.6 Measurement session reset remains isolated

已有 `prepareStart / completeStart(success)` 清 anchor 的行为保持。

跨 measurement session 不得根据旧 seq 计算 gap。

---

## 8. Do not alter SourceCore admission / assembly

本次 reset recovery 只属于 gap observability tracker。

不得为了该统计需求改变：

- packet admission；
- `RecentTrigger` reject 规则；
- Duplicate / OffsetOutside；
- TriggerSwitch / Timeout assembly；
- pending sync；
- startup buffering；
- SourceCore complete/partial delivery；
- PhysicalRoundNormalizer。

尤其不要把 reset recovery 解释成 SourceCore 业务层的“新 measurement session”；它只是同一 session 内 gap anchor 的恢复。

---

## 9. Partial-trigger semantics unchanged

既有冻结口径保持：

```text
partial trigger:
triggersPartial += 1
packetsDropped += missing packets inside that observed trigger
missingTriggerCount += 0
```

如果旧 trigger 在发生大幅 reset 前本身是 partial：

- 旧 trigger 的 partial missing packets 仍按现有 TriggerSwitch/Timeout 统计；
- reset transition 不额外制造 full missing-trigger gap；
- reset trigger 建立新 anchor。

无需为本任务修改 `triggersPartial` 或 `runtimeIncomplete` 定义。

---

# Part II — Deterministic production-seam tests

## 10. Extend real PAimage production test

优先直接扩展：

`MC_410T_MultiCard/delivery/tests/paimage_production_gap_test.cpp`

必须继续走当前真实测试链：

```text
NetworkController
 -> Backend
 -> SocketReceiver
 -> SourceCore
 -> observationSink
 -> CardStats
```

不得通过独立 fake gap calculator 验收。

---

## 11. Required new scenarios

### B-ADD-7 — same-session large reset recovery

推荐 `expectedPackets = 1`：

```text
complete T1000
complete T10       // same measurement session, large reset
complete T14
```

断言至少包括：

在 T10 后：

```text
missingTriggerCount == 0
packetsDropped == 0
```

在 T14 后：

```text
missingTriggerCount == 3
packetsDropped == 3 * expectedPackets
```

证明 reset transition 自身不计 gap，而 reset 后统计重新工作。

### B-ADD-8 — threshold boundary

至少锁定 255 / 256 两侧行为。

可使用两个独立 scenario：

```text
A: T400 -> T145   // direct backJump = 255
   => no re-anchor
   -> T401
   => remains adjacent to T400; no gap
```

```text
B: T400 -> T144   // direct backJump = 256
   => reset recovery / re-anchor T144
   -> T148
   => missingTriggerCount += 3
```

等价测试序列可接受，但必须明确证明阈值边界，而不是只测一个远大于 256 的例子。

---

## 12. Existing production-gap regression must remain

B-ADD-1..6 全部继续 PASS：

```text
B-ADD-1 full gap T100->T104
B-ADD-2 partial + full gap
B-ADD-3 adjacent
B-ADD-4 uint16 wrap forward
B-ADD-5 small backstep / stale
B-ADD-6 measurement-session reset
```

特别注意本次加入 large-reset recovery 后不能破坏：

```text
T65534 -> T1 == +2
T105 -> T102 -> T106 不产生额外 gap
```

---

## 13. Legacy DataProcessor regressions

现有 DataProcessor B1-B4 必须继续 PASS。

本任务不删除、不重写 legacy/test gap accounting。

最终软件语义应为：

```text
DataProcessor legacy/test path:
  threshold 256 reset recovery preserved

PAimage production SourceCore path:
  threshold 256 gap-anchor reset recovery aligned
```

---

# Part III — HANDOFF correction

## 14. Fix stale Session C baseline wording

更新：

`HANDOFF_SESSION_B_20260917.md`

当前 Session C inputs 中不得再把旧 Session B 初始 commit `917a719...` 描述成最终可继承实现。

必须明确：

- 第一次 review addendum production-gap code commit `93a2b9ce5cc40133767f6248900526a825e9b7ad` 已是 Session B 必需实现的一部分；
- test-stability commit `55898159668cc366dc7a1dd3f6767efa1adf01b7` 已在分支上；
- 本次 second-addendum 的新 reset-recovery code commit 也必须包含；
- Session C **必须从本 addendum 完成后的 `origin/codex/session-b-ui-observability-20260917` 最终远端 HEAD 开始**，不得从 `917a719...`、`5f6d3ff...` 或 `2566a028...` 回退开始。

由于文档 commit 无法无穷自引用自身最终 SHA，HANDOFF 中不要伪造“文档内精确 final HEAD”。应写清：

```text
Session C baseline = final remote HEAD of
origin/codex/session-b-ui-observability-20260917
after this second review addendum;
exact SHA is provided by the final execution report and must be verified by git fetch/rev-parse before Session C starts.
```

下一阶段 Session C 正式任务书将再次锁定这个 exact final SHA。

---

## 15. Fix stale test summary

HANDOFF 中旧的：

```text
SESSION_B_AUTOMATED_TESTS = PASS (ctest 41/41)
```

必须更新为本 addendum 实际最终 suite 数量和结果。

当前第一次 addendum 后 suite 为 42 tests；若本次只扩展已有 `paimage_production_gap_test` 而不新增 CTest target，通常仍应为：

```text
ctest 42/42 PASS
```

但必须以真实最终 `ctest` 输出为准，不得机械写 42。

---

# Part IV — Scope boundaries

## 16. Prohibited scope

本 addendum 不得修改：

- `PhysicalRoundNormalizer` 状态机；
- `startupFilterTriggerCount` / `disableCountBoundary` semantics；
- RingConfigDialog / RoundPolicySettings；
- 状态栏 `缺失 / 跳号数 / 丢包 / 已采集 / 已过滤` 口径；
- Session C imaging cap；
- timeout screenshot；
- FileSaver timeout round-folder rollover；
- RingBlockAssembler timeout reset sequencing；
- ImagingSvc reset/finalization；
- CUDA geometry / angle modulo / partial block；
- `sourceRoundComplete` / `reconstructionComplete` / `expectedBlocks`。

不要借本任务进行 SourceCore 大型重构。

---

## 17. Expected changed files

生产代码预计主要限于：

```text
MC_410T_MultiCard/delivery/src/PaimageAcquisition/SourceCore.cpp
```

必要时为了命名常量可最小修改：

```text
MC_410T_MultiCard/delivery/include/PaimageAcquisition/SourceCore.h
```

测试：

```text
MC_410T_MultiCard/delivery/tests/paimage_production_gap_test.cpp
```

文档：

```text
HANDOFF_SESSION_B_20260917.md
SESSION_B_REVIEW_ADDENDUM2_RECEIPT_20260917.md
```

如果 production diff 扩展到 UI/Ring/FileSaver/CUDA/Normalizer，属于范围漂移，应记录 blocker 而不是扩张实现。

---

# Part V — Validation / build / receipt

## 18. Required tests

在最终代码 commit 上至少执行：

1. focal production test：

```text
paimage_production_gap_test
```

并确认 B-ADD-1..8 全 PASS；

2. Session B direct regressions：

```text
data_processor_batch_test
card_status_formatting_test
round_policy_settings_test
network_diagnostics_test
physical_round_normalizer_test
paimage_host_output_test
```

3. 完整 CTest suite：

```powershell
ctest --output-on-failure -j 4
```

如果固定端口测试仍使用已经加入的 CTest `RESOURCE_LOCK`，保持该测试稳定性修复，不删除。

记录真实命令、test count、PASS/FAIL。

---

## 19. Windows build

在本 addendum 的最终**代码** commit 上按最新：

`origin/main:BUILD_STANDARD.md`

重新执行标准 Windows build：

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging\MC_410T_MultiCard\delivery"
cmd /c build_mingw_debug.cmd
```

记录：

- exact code SHA；
- configure/build preset；
- exit result；
- BUILD_STANDARD 要求的核心 exe 产物。

之后如仅追加 receipt/HANDOFF 文档 commit，不需要为纯文档 commit 再重建二进制，但最终报告必须区分 code SHA 与 final remote HEAD。

---

## 20. Required second-addendum receipt

新增：

`SESSION_B_REVIEW_ADDENDUM2_RECEIPT_20260917.md`

至少记录：

### Git identity

```text
branch = codex/session-b-ui-observability-20260917
starting SHA = 2566a028a590ec2e099a12dd00977f704c5e38e0
reset-recovery code commit
final remote HEAD（最终回复带外上报）
tracked git status
local HEAD / remote HEAD verification
```

### Root cause

说明：

- 第一次 addendum 的 production gap anchor 对 `delta <= 0` 一律不移动；
- small backstep 是正确行为；
- 但 large same-session sequence reset 需要重新建立 anchor，否则后续 gap accounting 失明。

### Final semantics

记录真实实现位置和判断顺序：

```text
signed forward/wrap first
small backstep second
large direct backJump >= 256 => re-anchor without gap
```

### Tests / build

记录 B-ADD-7、B-ADD-8、B-ADD-1..6 regression、完整 CTest、Windows build 的真实命令和结果。

---

## 21. Acceptance criteria

本 second addendum 只有以下全部满足才可关闭 Session B：

1. production SourceCore gap tracker 支持同-session large triggerSeq reset recovery；
2. threshold 精确为 256，与冻结 DataProcessor semantics 对齐；
3. reset transition 本身不增加 `missingTriggerCount`；
4. reset transition 本身不增加 full-trigger `packetsDropped` equivalents；
5. reset 后新 anchor 立即生效，后续 forward gap 正常累计；
6. backJump 255 不 re-anchor；
7. backJump 256 re-anchor；
8. uint16 wrap-forward 不误判成 reset；
9. small late/backstep 不误判成 reset；
10. measurement-session reset 仍隔离旧 anchor；
11. packet admission/assembly/sync/Normalizer 行为不变；
12. B-ADD-1..8 production test 全 PASS；
13. DataProcessor legacy B1-B4 继续 PASS；
14. Session A/B direct regressions PASS；
15. 完整 CTest PASS；
16. Windows full build PASS；
17. HANDOFF 不再引用旧 `917a719...` 作为 Session C 最终基线；
18. HANDOFF test count 与真实最终 suite 一致；
19. receipt + HANDOFF 已 push；
20. tracked working tree clean；
21. final report 确认 local HEAD == remote branch HEAD；
22. hardware validation 仍保持 PENDING。

---

## 22. Required final execution report

Codex 完成后报告：

```text
Task document
Branch = codex/session-b-ui-observability-20260917
Starting SHA = 2566a028a590ec2e099a12dd00977f704c5e38e0
Reset-recovery code commit
Final remote HEAD
Changed files
Exact reset-recovery logic / threshold
B-ADD-7 result
B-ADD-8 threshold-boundary result
B-ADD-1..6 regression result
Full CTest command + final test count/result
Windows build command + result + exact code SHA
SESSION_B_REVIEW_ADDENDUM2_RECEIPT_20260917.md
Updated HANDOFF_SESSION_B_20260917.md
Tracked git status
Local HEAD == remote HEAD
Hardware validation = PENDING
```

---

## 23. Hardware/system validation boundary

即使本 addendum 全部通过，也只表示软件层收口：

```text
SESSION_B_SOFTWARE_IMPLEMENTATION = PASS
SESSION_B_PAIMAGE_PRODUCTION_GAP_ACCOUNTING = PASS
SESSION_B_SAME_SESSION_SEQ_RESET_RECOVERY = PASS
SESSION_B_AUTOMATED_TESTS = PASS
SESSION_B_WINDOWS_BUILD = PASS
```

仍保持：

```text
PHYSICAL_ROUND_HARDWARE_VALIDATION = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS = NOT PROVEN BY THIS TASK
```

不要将 loopback/自动测试结果描述为真实 FPGA/NIC/LabVIEW 实机验证。
