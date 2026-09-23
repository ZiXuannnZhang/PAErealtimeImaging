# bug1 前端刷新闸门 4000 写死 + bug2 自动保存数据被截断覆盖 — 验证回执

- 分支：`codex/save-gate-bugfix-20260924`
- 起点：`40102eae6f78f61c4d5ad4e4a32e7ce4c82f90d6`（`codex/frontend-filter-stage-20260923` tip）
- `IMPLEMENTATION_SOURCE_SHA` = `466074030a9e82aac0fa6569b8a1fdc5d8e29a82`
- `REPORT_HEAD` = 本提交（receipt-only）
- 日期：2026-09-24（UTC+8）

来源：零相位滤波已过实机验收、暂未发现异常；验收过程中意外暴露两个与滤波无关的
历史 bug。本轮只做软件层正确性 + 自动化测试，实机验证由用户执行（见第 7 节）。

---

## 0. 用户已拍板的口径

| 项 | 决定 |
|---|---|
| bug1 阈值公式 | **单圈总A-line数 / 启用通道数**（= `RingReconCudaConfig.alinesPerFrame / enabledChannelCount`） |
| bug2 现场形态 | 自动保存三级数文件夹 `002`，其最后写入的 `.dat` 被**截断重写** |
| 修复范围 | **根因 + 全部"覆盖类"隐患**；静默丢数据类只出报告不改代码 |

---

## 1. bug1 根因

### 1.1 闸门本体（现象与它逐字吻合）

`HostOutput::sync()`（`MC_410T_MultiCard/delivery/src/PaimageAcquisition/HostOutput.cpp:88`）：

```cpp
if (normalizer_->disableCountBoundary() && ... &&
    classification.logicalTriggerIndex >= normalizer_->configuredLogicalTriggersPerRound())
    filter = true;   // 整个 sync 帧被丢弃
```

**路径分工解释了"显示+成像停、保存不停"**：

| 入口 | 派发 | 消费者 |
|---|---|---|
| `HostOutput::card()` | `deliverAssembled(group, save=true, frontend=false)` | FileSaver |
| `HostOutput::sync()` | `deliverAssembled(group, save=false, frontend=true)` | DisplayBuffer / Ring |

所以闸门只挡 `sync()` = 只挡时域/频域显示与实时成像，保存走 `card()` 完全不受影响。

`logicalTriggerIndex` 是**每圈全局触发序号**（`PhysicalRoundNormalizer::classify` 按
`(measurementSession, triggerSeq)` 去重，多卡同触发共用一个序号），阈值的物理含义
就是"每圈设计触发数"。这条 filter 正是那条兼容性设计：勾选「禁用计数重置」
（`disableCountBoundary`）后，超设计触发不再推前端，以免图像被意外重置。

### 1.2 阈值为什么是写死的 4000

| 位置 | 取值 |
|---|---|
| `AcqConfig.h:23` | `kDefaultLogicalTriggersPerRound = 4000` |
| `MainWindow.cpp`（监听启动前） | 从注册表 `AcquisitionParams/LogicalTriggersPerRound` 读入，缺省 4000 —— **全程序无任何 UI 控件可改** |
| `MainWindow.cpp:2550` | `cfg.logicalTriggersPerRound = m_logicalTriggersPerRound` |
| `NetworkControllerPaimage.cpp:243` | `m_config = config` → **整体覆盖** |
| `NetworkControllerPaimage.cpp:52` → `Backend.cpp:12` | 构造 `PhysicalRoundNormalizer(4000)` |

唯一按「单圈总A-line数」推导的地方是 `MainWindow::configureRingAssembler()`
里的 `setLogicalTriggersPerRound(cfg.alinesPerFrame / cfg.enabledChannelCount)`，
而它的调用点只有 `svcReady` 与 `onRealtimeImagingToggled(true)` 两处，**都以实时
成像已开启为前提**，且 `roundConfigValid` 不满足时整函数提前 return（连
`setLogicalTriggersPerRound` 都不执行）。

`AcqConfig.h` 原注释写明该值 "overridden by the ring configuration when ring mode
is active" —— 实现上这条覆盖**可被 `startPaimage` 的 `m_config = config` 抹掉、
且在成像开启前根本不存在**，于是闸门长期停在写死的 4000。

> **诚实标注**：闸门、4000 的来源、"环形推导值可被抹掉/可缺失"三条都是代码可证的。
> 用户实机那一次具体是哪条时序让 4000 留在生效状态，属运行期顺序问题，本轮未做实机
> 复现。修复不做单点补丁，而是让阈值**只有一个来源且不可能退回 4000**，一次性消除
> 全部相关时序。

### 1.3 公式依据

```text
每圈设计触发数 = 单圈总A-line数 / 启用通道数
```

与 `RingBlockAssembler.h:22-28` 的几何约定一致（每枚全局触发给每个启用通道 1 根
A-line，全局触发 g 奇偶交替 wl1/wl2），也与 `RingConfigDialog` 的
`alinesPerChannelPerFrame = total / (通道数 × 2)` 及其整除校验自洽。
用 `8000 / 2 通道 = 4000` 验算，与现场「7/4007」（设计 4000 + 7 枚额外）吻合。

---

## 2. bug1 修复

**唯一推导入口**：`AcqConfig.h` 新增纯函数

```cpp
inline int ringLogicalTriggersPerRound(int alinesPerFrame, int enabledChannelCount) noexcept;
// 几何非法（通道数<=0、总数<=0、不能整除、商<=0）返回 0，调用方保持原值不下发
```

> **与计划的一处偏离**：计划写"新增于 `RingConfigDialog.h`（或独立小头文件）"。
> 实际落在 `AcqConfig.h`——它就是 `AcqConfig::logicalTriggersPerRound` 的产生者，
> 与被赋值字段同处一处口径最不易漂移；更重要的是 `RingConfigDialog.h` 拉 `QDialog`，
> 放那里会让只链 `Qt6::Core` 的 `paimage_host_output_test` 无法直接断言该函数。

三处共用它，口径不可能再分叉：

1. **`RingConfigDialog::applyConfig()`** 成功后新增
   `emit ringRoundTriggersChanged(quint64)`（仿既有 `roundPolicyChanged` /
   `frontendFilterChanged` 模式）→ `MainWindow` 连接后
   `NetworkController::setLogicalTriggersPerRound()`。**改参数即生效**，不等实时成像
   开启、不等 `svcReady`。
2. **监听启动注入点**（`MainWindow` 构造 `AcqConfig` 处）：环形模式下取推导值写进
   `cfg.logicalTriggersPerRound`，无环形配置时才回退注册表值。这是消除
   `startPaimage` 的 `m_config = config` 覆盖的关键一改。推导为 0 时打日志告警，
   不再静默回退到 4000。
3. **`configureRingAssembler()`** 原地除法改为调用它，`roundConfigValid` 语义不变。

`kDefaultLogicalTriggersPerRound` 保留为线性模式/无环形配置的回退值，注释写明环形
模式下不得生效、且它没有 UI 入口。

**不动的部分**（兼容性设计本身保持原样）：`PhysicalRoundNormalizer`、
`HostOutput::sync` 的闸门语义、`disableCountBoundary` 语义。

---

## 3. bug2 根因

### 3.1 破坏性操作

`FileSaver::openNewFiles()`（`FileSaver.cpp:198`）：

```cpp
m_fileChannelA->open(QIODevice::WriteOnly)   // WriteOnly = 打开即截断同名已有文件
```

全链路**没有任何"文件已存在"检查或序号避让**。只要用旧文件名重开，先前落盘内容
立即消失。

### 3.2 触发序列（= 用户现场，已与用户逐条对齐）

1. 某轮数据写入 `002/Card*_Ch*_<suffix>_00K.dat`。
2. 自动保存边界提交成功 → `NetworkController::beginAutoSaveSession()` /
   `commitAutoSaveBoundary()` 调 `requestCloseSavers()`。
3. → `FileSaver::serviceCloseRequest()` → `closeFiles()`，**`m_fileSequence` 保持 K
   不变**（`run()` 的 `m_closeRequest` 分支同理）。
4. 十几分钟后，任何仍带**同一 `sessionGen`** 的帧到达（同轮迟到帧 / 残留帧 /
   `PhysicalRoundNormalizer::recentDecisions_` 命中缓存仍记旧 `roundGeneration` 的帧）。
5. `consumeTriggerGroup()` 见句柄为空 → 按**同一序号 K** `openNewFiles()` →
   **截断重写** `002` 里最后那个 `.dat`。

结果正是「002 最后写的 `.dat` 被十几分钟后写入的未知来源数据截断重写」。

---

## 4. bug2 修复 —— 落盘不变量「已写入磁盘的数据永不被改写」

### 4.1 承重改动（两道防线）

1. **`FileSaver::resolveFreeSequence()`**：自 `m_fileSequence` 起向后扫描，落位到第一个
   `ChA`/`ChB` **都不存在**的序号。所有开文件入口（`openNewFiles`、`startSaving`）都先走它。
   撞名被跳过时记一条 `FileRolloverInfo::reason == "sequence_collision"` 留痕。
2. **独占创建**：`QIODevice::WriteOnly | QIODevice::NewOnly`（Qt6 可用）。即使并发下
   扫描后仍撞名，也按打开失败告警，**绝不回退成截断**。

> **与计划的一处偏离**：计划第 3 条要求"关文件路径烧掉序号（写过数据就 `++`）"。
> **未采用**——因为 `openNewFiles` 会立即创建空文件，序号一经使用就已占用，
> `resolveFreeSequence()` + `NewOnly` 单独就足以保证不覆盖；而主动烧号会在文件编号上
> 留下空洞，打破既有用例与下游工具依赖的连号规律。同理 `startSaving` 处只做避让、
> 不做归零之外的额外推进。**不变量已完全达成，代价是编号保持连号。**

### 4.2 消除撞名诱因（策略清理）

- **`sourceIPv4` 变化不再 `m_fileSequence = 0`**。这一行既会把上一来源的 `_000` 顶掉，
  还会就地抵消物理轮次轮转刚做的 `++m_fileSequence`「安全推进」。新来源改落到下一个
  空闲序号。
- **会话代切换只在目录真的变化时才归零序号**。`gen == 0`（手动）或解析到同一目录时
  沿用当前序号，保持同目录内编号单调。`m_dropGen` fail-closed 行为不变。
- **`AutoSaveRoundCoordinator::commitBoundary()` 跳过磁盘上已存在的候选目录**。
  `directoryPreparer_` 默认实现是 `QDir::mkpath`，对已存在目录同样返回 `true`，只靠
  `prepared` 判定会把新会话写进旧文件夹，旧轮次的命名空间从此被两个会话共用。
- **最大编号扫描接受任意长度的纯数字文件夹名**。历史实现只认**恰好 3 字符**，
  `1000` 号段（以及任何被改名的文件夹）直接失踪，下次启动把编号退回去复用旧文件夹。
  该函数上收为 `AutoSaveRoundCoordinator::scanMaxDirectoryNumber()`（可独立测试），
  `MainWindow::initAutoSaveSavers()` 改为调用它。

> **与计划的一处小偏离**：计划把扫描修正在 `MainWindow::scanMaxAutoFolder()` 里就地改，
> 但该函数是 `MainWindow.cpp` 的文件静态，无法被测试调用，计划里的"两层都要测"落不了地。
> 上收到协调器是让它可测的最小改法，语义不变。

### 4.3 不改的部分

`bindMeasurementSession` 复用上一测量末代目录这一**策略**不改：有了 4.1 的独占创建后
复用不再有害（新数据落到新序号）。仅在此记录该行为。

---

## 5. 同类隐患清单 —— 本轮只报告，不改代码

均属**静默丢数据**而非覆盖，按用户选定范围移出本轮实现：

| # | 位置 | 问题 |
|---|---|---|
| H1 | `FileSaver::flushWriteBuffers()` | 文件未打开/写失败时仍 `clear()` 掉 `m_writeAccumA/B` → 已积累触发静默蒸发 |
| H2 | `FileSaver::flushWriteBuffers()` | `QFile::write()` 返回值全丢（磁盘满/IO 错误无感知） |
| H3 | `FileSaver::closeFiles()` | 注释写"刷盘"，实际只 `QFile::close()`（到 OS 页缓存），无 `flush()`/落盘保证 |
| H4 | `FileSaver::saveTriggerGroup()` | 旧队列路径满 `FILESAVER_QUEUE_SIZE` 时静默丢弃且无计数 |
| H5 | `DataProcessor::deliverAssembled()` | 旧队列路径用 `currentGeneration()` 盖 `sessionGen`，弱于 PAimage 路径的 `resolveRound(...)` 逐轮绑定 |
| H6 | `FileSaver` 成员 | `m_fileChannel*` / `m_writeAccum*` / `m_fileSequence` / `m_currentGen` 自身无同步，安全性完全依赖"所有调用方串在同一 saving worker 上"；建议写成显式不变量并加断言 |

---

## 6. 测试

### 6.1 新增/改写契约

**`filesaver_round_boundary_test`**（bug2 主契约）

| 用例 | 断言 |
|---|---|
| **T11**（bug2 回归主契约） | `requestClose()`+`serviceCloseRequest()` 后，同 `sessionGen` 晚到帧 ⇒ 原 `.dat` **逐字节不变**（A/B 双通道），新数据落新序号 |
| **T12** | `sourceIPv4` 变化 ⇒ 序号不归零，且不抵消物理轮次的 `++m_fileSequence` |
| **T13** | `sessionGen` 变化但解析目录不变 ⇒ 不截断 `_000` 且编号继续；目录变化 ⇒ 新目录从空闲序号起 |
| **T10**（改写） | `startSaving` 复用已含数据的目录 ⇒ 不截断 |

> **T10 有一条断言必须改**：原 `"T10 second measurement owns file 000 fresh"` 明确断言
> 第二次 `startSaving` **截断复用** `_000`——它把 bug2 的行为固化成了契约。按用户拍板的
> 「已落盘数据永不被改写」不变量改为：测量 1 保住 `_000`，测量 2 落到下一空闲序号。
> T10 的其余语义（不继承旧轮次状态、resume 后恰好一次轮转、轮转元数据）原样保留。

**`auto_save_round_coordinator_test`**（目录撞号）

| 用例 | 断言 |
|---|---|
| **A7** | 磁盘已有 `001`/`002`/`1000` 时，分配必须跳过；即使 `lastDirectoryNumber` 被低估（历史扫描漏掉 1000 号段的故障模式），`commitBoundary` 仍跳过已存在目录 |
| **A8** | `scanMaxDirectoryNumber` 看得见 `1000` 号段、忽略非数字名、空目录返回 0 |

**`paimage_host_output_test`**（bug1 闸门 + 推导）

| 用例 | 断言 |
|---|---|
| **G1** | 阈值 5 时，12 枚触发中只有 5 枚进前端；**同批 raw save 12 枚一枚不少**（直接断言"显示停/保存不停"这对现象分离） |
| **G2** | `setConfiguredLogicalTriggersPerRound(20)` 后闸门立即跟随，无需轮次/会话边界或服务重启 |
| **G3** | `ringLogicalTriggersPerRound` 纯函数：`8000/2=4000`、`20000/2=10000`（随参数缩放而非停在 4000）、`8000/1=8000`、非整除/零输入返回 0 |

### 6.2 结果

```text
targeted 13/13 PASS   （含 6 条上一轮 targeted 全部保持 PASS）
full      44/44 PASS   0 FAIL / 0 SKIP / 0 NOT_RUN
ctest -N Total Tests: 44   —— 与上一轮一致：本轮未新增/删除 add_test 注册项
```

### 6.3 判别力反证

把 `FileSaver.cpp` 整体换回修复前版本并重建后运行 `filesaver_round_boundary_test`：

```text
14 FAILURE(S)，全部落在覆盖类断言（T10/T11/T12/T13）
与覆盖无关的断言（轮次轮转计数、轮转元数据、T13 新目录起号、T11 封存前内容）仍然 PASS
```

即：这批断言确实只对"覆盖"有判别力，不是恒真式。反证后已还原修复版并重跑
build + 全量 ctest = 44/44 PASS。

### 6.4 覆盖不到的一层（诚实标注）

bug1 的**阈值接线**（`RingConfigDialog` 信号 → `NetworkController`、监听启动注入
`AcqConfig`）位于 MainWindow/UI 层，本仓没有可驱动它的单元测试宿主。本轮以 G3 纯函数
断言 + G1/G2 闸门行为断言 + 代码路径复核覆盖，接线正确性需由第 7 节实机清单确认。

---

## 7. 实机复核清单（由用户执行；本轮只给可判定的通过条件）

**bug1**
- 把「单圈总A-line数」设成使每圈触发数 > 4000（例如 2 卡 / 20000 → 10000），
  勾选「禁用计数重置」，跑一整圈。
- **通过条件**：时域/频域显示与实时成像**持续刷新越过 4000**，只在达到
  `单圈总A-line数 / 启用通道数` 后停止刷新；保存全程不间断。
- 反向确认：改「单圈总A-line数」后重跑，停止刷新的触发数应随之改变。

**bug2**
- 开自动保存连跑多轮，中途空置十几分钟再触发一轮。
- **通过条件**：抽查各三级数文件夹内每个 `.dat` 的 size/mtime，**旧文件不得出现晚于其
  所在轮次的修改**；晚到数据必须出现在新序号文件里。
- 若现场仍见到旧文件被改，取 `FileSaver` 的 `fileRolled(reason="sequence_collision")`
  与 `paimage.save` 诊断事件一并留存。

---

## 8. 证据口径

按项目约定分层表述。本轮只声称：

```text
source/code correctness                  = 是
automated build / test / selftest        = 是（13/13 定向 + 44/44 全量）
real hardware validation                 = 未声称（见第 7 节，由用户执行）
hardware root-cause attribution          = 未声称
```

`7 / 4007` 继续只作现场观察/操作配置，不升格为协议常量；额外 startup trigger 的
FPGA/LabVIEW 精确来源仍是 `NOT_PROVEN`。零相位滤波本身的实机验收结论不因本轮改动
而改变——本轮未触碰滤波的任何数值、参数或插入点。

---

## 9. 证据目录

```text
CODEX_REPORTS/save-gate-bugfix-20260924/
  validation-receipt.md    本文件
  build-summary.txt        构建 + BuildIdentity + 测试树 + 判别力反证
  ctest-targeted.txt       13 条定向回归
  ctest-full.txt           ctest -N + 44 条全量
  git-receipt.txt          分支 / SHA / tracked-clean / diff --stat
```

`REPORT_HEAD` 相对 `IMPLEMENTATION_SOURCE_SHA` 的 diff 只能包含该目录。
