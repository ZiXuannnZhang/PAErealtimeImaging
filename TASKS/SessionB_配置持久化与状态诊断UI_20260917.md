# Session B — 配置持久化与状态诊断 UI

## 1. Objective

本任务是四阶段串行改造中的 **Session B**。

Session A 已完成并通过独立软件审查。Session B 只负责把已经存在的核心轮次策略接入用户配置/默认值链，并完成工作三的统计与状态展示。

本阶段实现三类内容：

1. `startupFilterTriggerCount` 的环形扫描 UI、当前运行配置和“设为默认/恢复默认”持久化；
2. `disableCountBoundary` 的独立 checkbox、当前运行配置和“设为默认/恢复默认”持久化；
3. `missingTriggerCount`（“跳号数”）与前端 `缺失 / 跳号数 / 已采集 / 已过滤 / 丢包` 的冻结显示语义。

本阶段**不实现 Session C 的可变长度轮次生产收尾行为**，包括 configured count 后的 imaging cap、timeout 截图、FileSaver timeout 分轮、Ring/ImagingSvc timeout reset 新行为等。

---

## 2. Required preflight / governance

开始修改前读取：

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging"
git fetch --prune origin
git show origin/main:PROJECT_STATUS.md
git show origin/main:REPOSITORY_BASELINE.md
git show origin/main:BUILD_STANDARD.md
git show origin/codex/task-docs:TASKS/SessionB_配置持久化与状态诊断UI_20260917.md
git show origin/codex/session-a-round-policy-core-20260917-114452:HANDOFF_SESSION_A_20260917.md
git show origin/codex/session-a-round-policy-core-20260917-114452:SESSION_A_EXECUTION_RECEIPT_20260917.md
```

根目录旧 handoff/report 仅供追溯，不覆盖上述当前文档。

---

## 3. Exact baseline

Session B 必须从已经批准的 Session A 最终远端 HEAD 开始：

```text
baseline branch = codex/session-a-round-policy-core-20260917-114452
starting SHA    = eb283f64574d9043f4d4823338c31767c3b811fe
```

这是对一般 main baseline 的 task-specific override。

开始时：

```powershell
git switch codex/session-a-round-policy-core-20260917-114452
git merge --ff-only origin/codex/session-a-round-policy-core-20260917-114452
git rev-parse HEAD
```

必须确认 HEAD 精确等于：

`eb283f64574d9043f4d4823338c31767c3b811fe`

若远端已移动，记录差异并停止本任务，等待 baseline 重新确认。

---

## 4. Target implementation branch

从 exact starting SHA 新建：

`codex/session-b-ui-observability-20260917`

不得：

- 从 `main` 或 `codex/task-docs` 开始实现；
- 改写 Session A 分支历史；
- merge/rebase/cherry-pick 最新 main 源码到本实现分支；
- force push。

---

## 5. Session A interfaces that must be consumed, not reimplemented

Session A 已提供并通过审查：

```cpp
NetworkController::setStartupFilterTriggerCount(std::uint64_t)
NetworkController::setDisableCountBoundary(bool)
NetworkController::physicalRoundSnapshot() const
```

Normalizer Snapshot 已有：

```text
startupFilterTriggerCount
disableCountBoundary
currentPhysicalDistinctCount
currentStartupFilteredCount
lastCompletedPhysicalDistinctCount
lastCompletedStartupFilteredCount
```

Session B 必须消费这些 production API。

不要绕过 `NetworkController` 访问 Normalizer 私有成员，也不要重新实现 PhysicalRoundNormalizer 状态机。

---

# Part I — 环形配置 UI 与默认值持久化

## 6. UI placement

源码复核确认：

- `RingConfigDialog` 已承载 `单圈总A-line数` 和 `超时重置`；
- 构造时调用 `restoreDefaults()`；
- “设为默认”调用 `saveDefaults()`；
- 默认参数存放在同一个 `QSettings` group：

```text
RingConfigDialog/Defaults
```

因此本任务两个新控件必须放入 `RingConfigDialog` 的扫描/轮次相关区域，并接入该**现有**默认值链。

不要另建独立 settings namespace。

---

## 7. `startupFilterTriggerCount` UI

新增控件，用户可见标签必须是：

**`启动过滤触发数`**

推荐使用整数 SpinBox。

必须支持：

```text
0  = 不过滤启动 trigger
1  = 当前兼容行为
N  = 每 physical round 过滤前 N 个 new distinct physical trigger identities
```

工厂/无历史默认值：

`1`

UI tooltip 应明确：

- 单位是 distinct physical trigger，不是 packet/card 数；
- 多卡看到同一 trigger 只消耗一个过滤名额；
- 0 表示不做 startup filtering。

不要在 UI 文案中声称这是 FPGA/LabVIEW 已定义的协议 control trigger。

---

## 8. `disableCountBoundary` UI

新增独立 checkbox，用户可见文本必须精确为：

**`禁用计数重置`**

工厂/无历史默认值：

`false`（未勾选）

语义来自 Session A：

- 未勾选：configured logical count 仍产生现有 CountBoundary；
- 勾选：达到 configured logical count 不产生 fixed-count boundary，physical round 继续到 timeout。

该 checkbox 与 `startupFilterTriggerCount` 完全独立。

禁止根据：

`startupFilterTriggerCount == 0`

自动推导或修改 `disableCountBoundary`，反之亦然。

Tooltip 可以说明 timeout 仍是轮次边界，但**不要在 Session B 中实现 Session C 的 imaging/save 收尾逻辑**。

---

## 9. Existing “设为默认 / 恢复默认” contract

必须把两个值加入现有：

`RingConfigDialog/Defaults`

建议 key：

```text
startupFilterTriggerCount
disableCountBoundary
```

要求：

### 9.1 No saved history

首次/无对应 key：

```text
startupFilterTriggerCount = 1
disableCountBoundary = false
```

### 9.2 “设为默认”

用户修改控件后点击现有 **“设为默认”**：

- 当前两个控件值与 RingConfigDialog 其他默认参数一起写入现有 Defaults group；
- 下一次启动程序 / 新建该配置窗口时读取该保存值；
- 不得静默回退到 compiled 1/false，除非对应 key 从未保存。

### 9.3 “恢复默认”

沿用当前窗口语义：

- 若用户此前“设为默认”保存过值，恢复该已保存默认；
- 若没有历史值，恢复工厂 1/false。

### 9.4 Single persistence source

MainWindow 若需要在 `RingConfigDialog` 尚未创建时读取 policy，以保证 backend 创建前已经获得保存的默认值，必须读取**同一** `RingConfigDialog/Defaults` keys。

推荐提取一个很小、可测试的 policy settings helper，供 `RingConfigDialog` 和 MainWindow 共同使用；也可以采用其他等价的小范围方案。

禁止在第二个 QSettings group 再保存一份相同 policy，避免双源失步。

---

## 10. Runtime propagation requirement

仅把值显示/保存到 QSettings 不算完成。

两个参数必须实际传播到 Session A 已有 production API：

```cpp
NetworkController::setStartupFilterTriggerCount(...)
NetworkController::setDisableCountBoundary(...)
```

必须满足两类场景。

### 10.1 Program/backend startup

即使本次程序启动后用户尚未手动打开 `RingConfigDialog`，已经通过“设为默认”保存的 policy 也必须能在创建/配置 PAimage backend 时作为本次 runtime policy 使用。

即：保存 default 后，未来启动不能因为对话框 lazy-create 而悄悄使用 1/false。

### 10.2 Current-session Apply/OK

用户在 RingConfigDialog 中修改两个控件并执行现有：

- `应用`
- 或 `确定`

成功应用配置后，应把当前 policy 同步给当前 `NetworkController`。

可以用小型 signal/helper/MainWindow forwarding 实现；具体 wiring 自行选择。

用户会主动避免在实际 acquisition 进行中改变这些值，因此**不需要**新增复杂 mid-round transactional reconfiguration。

不要为了本任务修改 Session A setter 的状态机语义。

---

# Part II — `missingTriggerCount` / “跳号数”

## 11. New CardStats field

在 `CardStats` 增加独立累计计数，推荐并优先使用名称：

`missingTriggerCount`

含义：

**由 forward `triggerSeq` gap 推断出的、完全 0 packet 到达的 missing trigger 数量累计值。**

这是 per-card cumulative observability。

必须加入：

- atomic hot field；
- `CardStats::Snapshot`；
- `snapshot()`；
- `NetworkController::runtimeStatsFields()` / 对应 runtime diagnostics JSON。

不要复用 `triggersDiscarded`。

---

## 12. Exact increment semantics

现有 `DataProcessor.cpp` 已有两个互斥的 full-trigger gap 入口，并已经在这些位置将完整缺失 trigger 换算进 `packetsDropped`。

### 12.1 Trigger switch path

现有：

```cpp
const int16_t skipGap =
    static_cast<int16_t>(pkt.triggerSeq - oldSeq) - 1;

if (skipGap > 0) {
    packetsDropped += skipGap * expectedPackets;
}
```

同一个 `if (skipGap > 0)` 中增加：

```text
missingTriggerCount += skipGap
```

例如：

```text
old T100 -> next observed T104
skipGap = 3
missingTriggerCount += 3
```

不是 +1 gap event。

### 12.2 Empty-buffer / last-flushed anchor path

现有：

```cpp
const int16_t gap =
    static_cast<int16_t>(pkt.triggerSeq - m_lastFlushedTriggerSeq) - 1;

if (gap > 0) {
    packetsDropped += gap * expectedPackets;
}
```

在同一正 gap 分支增加：

```text
missingTriggerCount += gap
```

`didSwitch` 已用于避免同一 transition 被两条路径重复处理；不要另建第三套 gap 判定。

---

## 13. Do not alter packet-loss decisions

本任务新增的是 observability counter，不改变现有 packet handling。

必须保持：

- signed 16-bit wrap-aware gap 判断；
- `packetsDropped` 现有累加方式；
- partial trigger 的缺包计算；
- stale/backstep/reset recovery；
- packet queue / assembly / flush 行为。

### Partial trigger rule

若一个 trigger 有部分 packet 到达，但切换/timeout 前未完整：

```text
triggersPartial += 1
packetsDropped += missing packets within that trigger
missingTriggerCount += 0
```

`missingTriggerCount` 只表示整个 trigger 完全缺失的数量。

---

# Part III — 状态栏语义

## 14. Frozen user-visible meanings

前端字段必须使用以下固定口径。

### `缺失`

来源：

`CardStats::triggersPartial`

含义：有部分 packet 到达，但最终未完整组装的 trigger 数。

### `跳号数`

来源：

`CardStats::missingTriggerCount`

含义：由 forward trigger sequence gap 推断的**完全缺失 trigger 数量**。

例如 T100 -> T104：显示累计增加 3。

### `丢包`

来源：

现有 `CardStats::packetsDropped`

含义保持当前实现：

- partial trigger 内缺失 packet；
- 完整 missing trigger 的 `gap * expectedPackets` packet equivalents。

不要把 `丢包` 改成 trigger 数。

### `已采集`

来源必须是：

`PhysicalRoundNormalizer::Snapshot`

不是 CardStats，不是 logical accepted count。

含义：当前 physical round 已观察到的 distinct physical trigger 数，**包含随后被 startup filter 过滤的 trigger**。

### `已过滤`

来源必须是同一个 Normalizer Snapshot 的 startup filtered round count。

含义：当前 physical round 实际已过滤的 distinct physical trigger 数。

---

## 15. Status line layout

主状态文本采用紧凑口径：

```text
卡1 | 缺失: 0 | 跳号数: 0 | 已采集: 4007
```

因此修改当前：

`卡%1 | 报文不完整触发: %2`

为上述冻结语义。

不要在主行永久增加 `已过滤`；它放 tooltip。

---

## 16. Round-level count display selection

Session A 在 boundary 后会将 current counters 清零，并 latch：

```text
lastCompletedPhysicalDistinctCount
lastCompletedStartupFilteredCount
```

为了避免刚结束一轮后 UI 立即闪成 0，Session B 使用以下简单显示规则：

```text
if currentPhysicalDistinctCount > 0:
    已采集 = currentPhysicalDistinctCount
    已过滤 = currentStartupFilteredCount
else:
    已采集 = lastCompletedPhysicalDistinctCount
    已过滤 = lastCompletedStartupFilteredCount
```

这只是 presentation fallback，不改变 Normalizer 真实 counters。

一旦新一轮第一枚 physical distinct 出现，current physical > 0，应立即切回新轮 current values。

`已采集/已过滤` 是 global round-level 数据；如果沿用每卡状态 QLabel 展示，它们在各卡行相同是预期行为。

---

## 17. Tooltip

现有每卡状态使用一个 QLabel，因此不要求为了“悬停已采集”拆成多个独立子 label。

现有状态 QLabel 的 tooltip 至少加入：

```text
已过滤: <round filtered count>
丢包: <packetsDropped>
```

并保留有价值的现有详细诊断：

- 触发完成；
- 输入/保存队列；
- Socket接收；
- Processor出队；
- 批边界丢弃；
- 速率/触发率；
- triggersDiscarded / saveQueueDiscards 等。

当前 tooltip 的 `不完整触发缺包数` 实际读取 `packetsDropped`，而 `packetsDropped` 还包含 full-trigger gap equivalents，因此该旧文案必须改成准确的 **`丢包`** 或等价明确表述。

不要改变 `packetsDropped` 数据本身。

---

## 18. Formatting architecture

`已采集/已过滤` 是 global Normalizer round state，不能写入或伪装成 per-card `CardStats`。

推荐：

- 给 `CardStatusFormatting` 增加一个很小的 round display struct；或
- 给 `text()/tooltip()` 增加明确的 physical/filtered 参数。

`MainWindow::onUpdateStatistics()` 每个 refresh tick 最多读取一次：

`m_netController->physicalRoundSnapshot()`

根据第 16 节规则求出 display physical/filtered，然后传给每卡格式化。

不要每个 card 重复查询 Normalizer，也不要在 UI 自己推算 distinct physical identities。

---

# Part IV — Tests

## 19. Deterministic missing-trigger tests

优先扩展现有 `data_processor_batch_test` 或最接近的 DataProcessor production test seam。

至少覆盖：

### B1 full gap quantity

构造等价：

```text
T100 -> T104
```

验证：

```text
missingTriggerCount += 3 exactly once
packetsDropped += 3 * expectedPackets
```

若 T100 本身同时是 partial，要分别验证 partial missing packet 与 full-gap packet equivalents 的总和，但 missingTriggerCount 仍只加 3。

### B2 partial trigger only

某 trigger 收到部分 packets 后切换：

```text
triggersPartial += 1
packetsDropped += missing packet quantity
missingTriggerCount unchanged
```

### B3 empty-buffer gap path

完整 trigger flush 后 buffer 为空，下一 trigger 跨 gap：

验证该第二条路径同样按 trigger 数累计且不双计。

### B4 uint16 wrap / backward cases

保留/补充检查：

- 正常 wrap forward semantics 不误计；
- backstep/reset recovery 不增加 missingTriggerCount。

---

## 20. Formatting tests

扩展 `card_status_formatting_test`。

至少固定：

```text
卡1 | 缺失: 3 | 跳号数: 4 | 已采集: 4007
```

并验证 tooltip 包含：

```text
已过滤: 7
丢包: <packetsDropped>
```

且不再使用 `不完整触发缺包数` 指代总 `packetsDropped`。

增加 current/last fallback helper 的 deterministic test（可在纯 formatting/helper 层完成）。

---

## 21. Settings/default tests

必须对以下行为提供 deterministic 软件证据：

```text
无历史 -> 1 / false
保存 default 7 / true
重新读取 -> 7 / true
restore -> 7 / true
```

以及两个参数互相独立，例如：

```text
X=0 + disable=false
X=7 + disable=true
```

测试不得依赖或污染用户真实 ini。

推荐将读取/写入逻辑抽成可接受临时 `QSettings`/临时 ini 的小 helper，再由 RingConfigDialog 和 MainWindow 共用。

不要为了测试建立第二套 production settings store。

---

## 22. Production propagation tests

至少证明：

1. 保存 default 后，即使 RingConfigDialog 尚未 lazy-create，MainWindow/controller 初始化路径仍能拿到已保存 policy；
2. Apply/OK 后实际调用/更新 `NetworkController` policy；
3. `NetworkController::physicalRoundSnapshot()` 能反映当前 policy。

如果完整 MainWindow widget test 成本过高，可以通过拆出的 settings/helper + 已有 NetworkController/HostOutput test seam 组合证明；不要为本任务构建大型 UI 自动化框架。

---

## 23. Session A regressions

至少重新运行：

```text
physical_round_normalizer_test
paimage_host_output_test
```

并运行本任务直接影响的：

```text
data_processor_batch_test
card_status_formatting_test
network_diagnostics_test（若 Windows test seam 可执行）
```

根据实际改动补充最相关测试。

不得只运行新测试。

---

# Part V — Scope boundaries

## 24. Prohibited Session C scope

Session B 不得实现：

- `logicalTriggerIndex >= configuredLogicalTriggersPerRound` 后停止 sync/display/Ring/CUDA；
- realtime imaging cap；
- timeout 时保存当前窗口图；
- timeout 时新旧 round FileSaver directory binding 切换；
- timeout 时新增 Ring reset sequencing；
- ImagingSvc timeout reset 新逻辑；
- partial Ring block；
- CUDA 动态 block；
- angle modulo 修改；
- `reconstructionComplete` / `sourceRoundComplete` / `expectedBlocks` 重定义。

这些全部留给 Session C。

---

## 25. Other prohibited scope

不要：

- 修改 PhysicalRoundNormalizer 分类状态机；
- 改 Session A 已批准的 X=0/1/N 或 disableCountBoundary semantics；
- 把 per-card gap count 与 global physical round count 混在一起；
- 把 `triggersDiscarded` 改叫或复用成“跳号数”；
- 改 packet loss 算法来迎合 UI；
- 进行无关 UI/架构重构；
- 声称本 Session 完成硬件验证。

---

# Part VI — Expected files / implementation freedom

## 26. Likely changed files

预计涉及：

```text
MC_410T_MultiCard/delivery/include/RingConfigDialog.h
MC_410T_MultiCard/delivery/src/RingConfigDialog.cpp
MC_410T_MultiCard/delivery/include/MainWindow.h
MC_410T_MultiCard/delivery/src/MainWindow.cpp
MC_410T_MultiCard/delivery/include/DataTypes.h
MC_410T_MultiCard/delivery/src/DataProcessor.cpp
MC_410T_MultiCard/delivery/include/CardStatusFormatting.h
MC_410T_MultiCard/delivery/src/NetworkController.cpp
MC_410T_MultiCard/delivery/tests/card_status_formatting_test.cpp
MC_410T_MultiCard/delivery/tests/data_processor_batch_test.cpp
MC_410T_MultiCard/delivery/tests/CMakeLists.txt
```

以及一个很小的 settings helper/test（如果采用该实现）。

这些只是预期范围，不要求机械修改每个文件。

实现方式可根据现有代码选择，但必须满足本 task observable semantics。

---

# Part VII — Build, evidence, handoff

## 27. Windows build

最终代码 commit 必须按最新：

`origin/main:BUILD_STANDARD.md`

执行标准 Windows configure + build。

默认入口：

```powershell
Set-Location "D:\ChatGPT\PAERealtimeImaging\MC_410T_MultiCard\delivery"
.\build_mingw_debug.cmd
```

记录真实命令、工具链、最终 code SHA、exit result 和要求的 exe 产物。

本任务不修改 CUDA core，不要求无条件重编 CUDA。

---

## 28. Required Session B execution receipt

新增：

`SESSION_B_EXECUTION_RECEIPT_20260917.md`

必须记录：

- repo / branch / exact starting SHA；
- implementation commit(s)；
- 每个 test 的真实命令与 exit/result；
- Windows build 的真实命令、code SHA、result；
- dependency source/hash 按 BUILD_STANDARD 适用范围记录；
- tracked `git status`；
- hardware validation boundary。

不要只写“tests passed”。

---

## 29. Required Session B handoff

新增：

`HANDOFF_SESSION_B_20260917.md`

至少包含：

### Repository state

- branch
- Session A base SHA = `eb283f64574d9043f4d4823338c31767c3b811fe`
- implementation code commit
- final remote HEAD（可在最终回执 out-of-band 报告）

### Implemented config path

准确记录：

- QSettings group/key；
- factory defaults；
- MainWindow/backend startup 如何读取；
- Apply/OK 如何传播至 NetworkController；
- 最终 getter/helper/signal 的真实名称。

### Stats semantics

准确记录：

- `missingTriggerCount` 增量位置；
- `缺失/跳号数/丢包/已采集/已过滤` 来源；
- current→lastCompleted fallback 规则。

### Tests/build

引用 execution receipt 并总结结果。

### Session C inputs

明确告诉下一会话：

- 从哪个 final HEAD 开始；
- `startupFilterTriggerCount` 和 `disableCountBoundary` 已可由用户配置并正确传至 controller；
- Session C 只需要实现 variable-length production data/imaging/save behavior；
- 不要重写 Session A/B 配置与统计语义。

---

## 30. Acceptance criteria

Session B 只有满足以下全部条件才可软件验收：

1. `启动过滤触发数` UI 存在，factory/no-history default=1；
2. `禁用计数重置` 独立 checkbox 存在，factory/no-history default=false；
3. 两者进入现有 `RingConfigDialog/Defaults` “设为默认/恢复默认”链；
4. 保存 default 后下一程序启动即使未打开 dialog，也不会静默退回 1/false；
5. Apply/OK 将当前 policy 传入 Session A `NetworkController` APIs；
6. 两参数完全独立；
7. `missingTriggerCount` 按完整 missing trigger 数量累计；
8. T100->T104 精确 +3，且 packetsDropped 原语义保持；
9. partial trigger 不增加 missingTriggerCount；
10. 主状态行精确表达 `缺失 / 跳号数 / 已采集`；
11. tooltip 包含 `已过滤` 和准确的 `丢包`；
12. `已采集/已过滤` 来自 Normalizer round snapshot，不来自 CardStats；
13. boundary 后 presentation fallback 到 lastCompleted，下一轮开始后切回 current；
14. runtime diagnostics 包含 missingTriggerCount；
15. required deterministic tests PASS；
16. Session A regressions PASS；
17. Windows build PASS；
18. implementation + HANDOFF + execution receipt 已 push；
19. tracked working tree clean；
20. 最终回复确认 local HEAD == remote branch HEAD；
21. hardware validation 明确保持 PENDING。

---

## 31. Final execution report

完成后报告：

```text
Task document
Implementation branch
Starting SHA = eb283f64574d9043f4d4823338c31767c3b811fe
Implementation commit(s)
Final remote HEAD
Changed files
QSettings keys/default semantics
Runtime policy propagation path
missingTriggerCount implementation locations
Final status text/tooltip semantics
Exact test commands + results
Exact Windows build command + result
SESSION_B_EXECUTION_RECEIPT_20260917.md
HANDOFF_SESSION_B_20260917.md
Tracked git status
Local HEAD == remote HEAD
Hardware validation = PENDING
```

不得将软件测试/build 结果描述为真实 FPGA/LabVIEW/硬件验证完成。
