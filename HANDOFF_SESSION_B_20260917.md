# Session B Handoff — 配置持久化与状态诊断 UI

## Repository state

- Repository: `ZiXuannnZhang/PAErealtimeImaging`
- Implementation branch: `codex/session-b-ui-observability-20260917`
- Session A base SHA: `eb283f64574d9043f4d4823338c31767c3b811fe`
  （`codex/session-a-round-policy-core-20260917-114452` 最终远端 HEAD，task-specific baseline override）
- Implementation code commit: `917a71986d01316a8dcb6a2707739b2a7be29d27`
- Review addendum code commit: `93a2b9ce5cc40133767f6248900526a825e9b7ad`
  （production 跳号统计修复；详见 `SESSION_B_REVIEW_ADDENDUM_RECEIPT_20260917.md`）
- Final remote HEAD: reported out-of-band in the final execution report / execution receipt
  （receipt commit 只含文档，不改动代码，避免自引用）。
- Tracked working tree: clean at the code commit and again before push.
- 未合并 `main`；hardware validation 保持 PENDING。

## Implemented config path

### QSettings 存储（单一持久化源）

- Group: `RingConfigDialog/Defaults`（现有“设为默认/恢复默认”链，未新建 namespace）
- Keys:
  - `startupFilterTriggerCount`（quint64，出厂/无历史默认 `1`）
  - `disableCountBoundary`（bool，出厂/无历史默认 `false`）
- 两个 key 独立读写，互不推导（`startupFilterTriggerCount==0` 不影响
  `disableCountBoundary`，反之亦然）。
- 共享 helper：`MC_410T_MultiCard/delivery/include/RoundPolicySettings.h`
  - `RoundPolicySettings::Policy{startupFilterTriggerCount, disableCountBoundary}`
  - `RoundPolicySettings::load(QSettings&)` / `save(QSettings&, const Policy&)`
    （调用方提供 QSettings，helper 自行 begin/end 上述 group；测试用临时 INI，
    不污染用户真实 INI）
  - `RoundPolicySettings::loadDefault()`：生产入口，读 `paimageSettingsPath()`

### UI 控件（RingConfigDialog 扫描参数页）

- `启动过滤触发数`：`m_spnStartupFilterTriggers`（QSpinBox，0..1000000）。
  单位是 distinct physical trigger（非 packet/卡数；多卡同一 trigger 只消耗
  一个过滤名额）；0=不过滤，1=兼容行为，N=过滤前 N 枚。
- `禁用计数重置`：`m_chkDisableCountBoundary`（QCheckBox）。勾选后达到配置
  逻辑触发数不产生固定计数边界，物理轮次继续到超时；超时仍是轮次边界。
- 构造时 `restoreDefaults()` 经 `RoundPolicySettings::load` 读取；`saveDefaults()`
  （“设为默认”）经 `RoundPolicySettings::save` 与其他默认参数同次落盘；
  “恢复默认”复用同一路径（有保存值恢复保存值，无保存值回退出厂 1/false）。

### Runtime 传播

- 程序/backend 启动：`MainWindow::startListeningWithIPs()` 在
  `new NetworkController(this)` 后立即调用
  `RoundPolicySettings::loadDefault()` 并下发
  `NetworkController::setStartupFilterTriggerCount / setDisableCountBoundary`。
  即使 RingConfigDialog 尚未 lazy-create，已保存 default 也在
  `createPaimageBackend()` 时经 controller 成员写入 `Backend::Settings`，
  不会静默退回 1/false。
- 当前会话 Apply/OK：`RingConfigDialog::applyConfig()` 成功后发出
  `roundPolicyChanged(quint64, bool)` 信号；`MainWindow::ensureRingConfigDialog()`
  （两处 lazy-create 的统一入口）把该信号转发到当前 `NetworkController`
  的上述两个 Session A production setter。成像启动路径的 `applyConfig()`
  直调同样经此信号传播。
- 未改动 Session A setter 的状态机语义；用户在实际采集中不修改这些值，
  未新增 mid-round transactional reconfiguration。

## Stats semantics

### `missingTriggerCount`（跳号数）

- 新 per-card 累计字段：`CardStats::missingTriggerCount`（atomic hot field），
  进入 `CardStats::Snapshot`、`CardStats::snapshot()` 与
  `NetworkController::runtimeStatsFields()`（runtime diagnostics JSON key
  `missingTriggerCount`）。未复用 `triggersDiscarded`。
- Production owner（review addendum 修复后）：`paimage::SourceCore` 在 production
  ingress 的 per-card assembly 激活点维护 forward trigger 锚点，发射一次
  `Decision::TriggerGap`（count = 完全缺失 trigger 数，T100->T104 记 3）；
  `NetworkControllerPaimage.cpp::observationSink` 消费该事件：
  `missingTriggerCount += count`、`packetsDropped += count * expectedPackets`。
  回退/迟到 trigger（int16 差值 <= 0）不计数也不移动锚点；锚点随 recent
  窗口生命周期，`prepareStart/completeStart` session 边界清除。
- Legacy/test path（保留，与 production 语义一致但只服务
  `DataProcessor::processInputBatch()` 包组装路径，production 不经过）：
  1. 触发切换路径 `if (skipGap > 0)`：`missingTriggerCount += skipGap`
     （T100->T104 记 +3）；
  2. 空缓冲区/last-flushed 锚点路径 `if (gap > 0)`：`missingTriggerCount += gap`。
  两条路径由既有 `didSwitch` 互斥，同一 transition 不双计；signed 16-bit
  wrap-aware 判断、`packetsDropped` 语义、partial/backstep/reset recovery 均未改。
- partial trigger 规则：有部分包到达但未完整 → `triggersPartial += 1` +
  包级 `packetsDropped`，`missingTriggerCount += 0`。

### 前端冻结口径

- 主状态行（紧凑）：
  `卡%1 | 缺失: <triggersPartial> | 跳号数: <missingTriggerCount> | 已采集: <round collected>`
- `缺失` = `CardStats::triggersPartial`（部分到达但未完整组装的 trigger 数）。
- `跳号数` = `CardStats::missingTriggerCount`（完全 0 包到达的 missing trigger 数）。
- `丢包` = `CardStats::packetsDropped` 原语义（partial 内缺包 + 完整 missing
  trigger 的 gap*expectedPackets 包当量）；tooltip 中旧文案
  `不完整触发缺包数` 已改为 `丢包`（数据未变）。
- `已采集` / `已过滤` = 全局轮次数据，来源是
  `NetworkController::physicalRoundSnapshot()`（Normalizer Snapshot 的
  current/lastCompleted physical/startup-filtered counters），不来自 CardStats。
- current→lastCompleted presentation fallback（`CardStatusFormatting::roundDisplayForUi`）：
  `currentPhysicalDistinctCount > 0` 时显示 current（含新一轮首枚 trigger 立即切回），
  否则显示 `lastCompletedPhysicalDistinctCount / lastCompletedStartupFilteredCount`，
  避免 boundary 后 UI 闪 0；不改变 Normalizer 真实 counters。
- `已过滤` 仅在每卡状态 QLabel 的 tooltip（`已过滤: <n>`），不进主行。
- 格式化架构：`CardStatusFormatting::RoundDisplay{collected, filtered}` +
  `text(cardNumber, stats, round)` / `tooltip(stats, round)`；
  `MainWindow::onUpdateStatistics()` 每个 refresh tick 只读取一次
  `physicalRoundSnapshot()`，算出 RoundDisplay 后传给每卡格式化
  （`MainWindow::formatCardStatusText / formatCardStatusTooltip` 透传）。

## Tests / build

详见 `SESSION_B_EXECUTION_RECEIPT_20260917.md`。摘要：

- `data_processor_batch_test`：新增 B1（T100->T104 切换路径精确 +3 且不双计）、
  B2（仅 partial 不计跳号）、B3（空缓冲区锚点路径按 trigger 累计）、
  B4（uint16 wrap forward / backstep / reset recovery 不计跳号）——PASS。
- `card_status_formatting_test`：冻结主行 `卡1 | 缺失: 3 | 跳号数: 4 | 已采集: 4007`、
  tooltip 含 `已过滤: 7` / `丢包: 7`、无 `不完整触发缺包数`、current/lastCompleted
  fallback helper——PASS。
- `round_policy_settings_test`（新增）：无历史->1/false、保存 7/true->重读/恢复
  7/true、X=0 与 disable 独立、key 级独立、临时 INI 不污染真实配置——PASS。
- `network_diagnostics_test`：`runtimeStatsFields` 增加 `missingTriggerCount` 映射；
  新增 `roundPolicyPropagation()`（controller setter->snapshot 反映 + 两参数独立
  + 默认 1/false）——PASS（`--legacy-control-only`，exit 0）。
- Session A 回归：`physical_round_normalizer_test`、`paimage_host_output_test` PASS；
  完整 ctest 套件 41/41 PASS。
- Review addendum（详见 `SESSION_B_REVIEW_ADDENDUM_RECEIPT_20260917.md`）：
  新增 `paimage_production_gap_test`，经 loopback UDP 走完整
  NetworkController→Backend→SocketReceiver→SourceCore→observationSink→CardStats
  production 路径，B-ADD-1..6（full gap T100->T104 记 +3 与 +3*expectedPackets、
  partial+full gap 并存、adjacent 不计、uint16 wrap T65534->T1 记 +2、
  backstep/stale 不制造假跳号、session reset 锚点隔离）PASS；
  DataProcessor legacy B1-B4 继续 PASS；完整 ctest 套件 42/42 PASS。
- Windows 构建：`cmd /c build_mingw_debug.cmd`（configure preset `mingw-debug`
  + build preset `mingw-debug-build`），exit 0，四个必需 exe 齐备。

## Session C inputs

- 从 `codex/session-b-ui-observability-20260917` 的最终远端 HEAD 开始（代码 commit
  `917a71986d01316a8dcb6a2707739b2a7be29d27` + 其上的文档 commit；以远端
  HEAD 为准并核对精确 SHA）。
- `startupFilterTriggerCount` 与 `disableCountBoundary` 已可由用户配置
  （RingConfigDialog UI + RingConfigDialog/Defaults 持久化），并经
  NetworkController/HostOutput/Backend::Settings 正确传至
  PhysicalRoundNormalizer；`physicalRoundSnapshot()` 可回读当前 policy。
- Session C 只需实现可变长度轮次的 production data/imaging/save 收尾行为
  （configured count 后 imaging cap、timeout 截图、FileSaver timeout 分轮、
  Ring/ImagingSvc timeout reset 新行为等）。
- 不要重写 Session A/B 的 core 分类状态机、配置链与统计语义：
  Normalizer 状态机、X=0/1/N 与 disableCountBoundary 语义、
  RoundPolicySettings 单一持久化源、missingTriggerCount/缺失/跳号数/丢包/
  已采集/已过滤 口径、current→lastCompleted fallback 均已冻结并通过审查链。
- `missingTriggerCount` / 完整 missing-trigger 的 `packetsDropped` 包当量统计
  已在 production PAimage ingress（SourceCore `Decision::TriggerGap` →
  NetworkControllerPaimage `observationSink`）完成，Session C 不需要也不应
  再实现一遍；DataProcessor `processInputBatch()` 中的同名逻辑是 legacy/test
  packet-assembly 路径的保留实现，production 数据不经过它。

## Hardware/system validation boundary

```text
SESSION_B_SOFTWARE_IMPLEMENTATION = PASS
SESSION_B_AUTOMATED_TESTS = PASS (ctest 41/41)
SESSION_B_WINDOWS_BUILD = PASS
PHYSICAL_ROUND_HARDWARE_VALIDATION = PENDING
FPGA/LABVIEW_TRIGGER_SEMANTICS = NOT_PROVEN_BY_SESSION_B
```

Session B 全部为软件侧 UI/持久化/观测性工作；不构成真实 FPGA/NIC/LabVIEW
硬件验证，也不得将软件测试通过解释为实机问题已解决。
